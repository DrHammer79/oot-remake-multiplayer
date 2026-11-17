"""Squadala! bug-report bot.

One slash command, /bugreport, in one server (the dev channel's server). A report is
saved under coord-multiplayer/player-logs/<user>-<UTC time>/, added to
coord-multiplayer/reports/player-reports.md as 'NEW (bot)' and posted to the
private dev channel. Everything the player types is stored and shown as quoted,
untrusted text.

No privileged intents and no message content: the bot only sees the slash
commands sent to it. Runs on the owner's PC (run-bot.ps1).

Usage: python bot.py --config <path to discord-bot.json>
"""

import argparse
import asyncio
import datetime as dt
import io
import logging
import logging.handlers
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import report_core as core  # noqa: E402

try:
    import discord
    from discord import app_commands
except ImportError:  # pragma: no cover - the tests never import this file
    print("discord.py is not installed; see tools/discord-bot/README.md", file=sys.stderr)
    sys.exit(3)

log = logging.getLogger("report-bot")

GENERIC_ERROR = "Sorry, something went wrong with your report. Please try again later."


def setup_logging(log_path, token):
    fmt = "%(asctime)s %(levelname)s %(name)s: %(message)s"
    handler = logging.handlers.RotatingFileHandler(log_path, maxBytes=1_000_000, backupCount=3, encoding="utf-8")
    handler.setFormatter(core.ScrubFormatter(token, fmt))
    console = logging.StreamHandler()
    console.setFormatter(core.ScrubFormatter(token, fmt))
    root = logging.getLogger()
    root.setLevel(logging.INFO)
    root.addHandler(handler)
    root.addHandler(console)


class ReportBot(discord.Client):
    def __init__(self, cfg):
        intents = discord.Intents.none()
        intents.guilds = True  # channel cache only; not privileged
        super().__init__(intents=intents, allowed_mentions=discord.AllowedMentions.none(),
                         application_id=cfg.get("application_id"))
        self.cfg = cfg
        self.guild_id = None  # the one server served; set in setup_hook
        self.tree = app_commands.CommandTree(self)
        self.tree.on_error = self.on_command_error
        self.user_limit = core.RateLimiter(core.USER_LIMIT, core.USER_WINDOW)
        self.global_limit = core.RateLimiter(core.GLOBAL_LIMIT, core.GLOBAL_WINDOW)
        self.lock = asyncio.Lock()
        self.logs_dir = cfg["coord_dir"] / "player-logs"
        self.tracker = cfg["coord_dir"] / "reports" / core.TRACKER_NAME

    async def setup_hook(self):
        # The server is the dev channel's server (the config's guild_id, if set, must match).
        # Fail closed: without it the bot stops; it never registers /bugreport globally.
        # Raises core.StartupError (caught in main, exit 2) when the server is unknown or the
        # report channel is on another server.
        self.guild_id = core.resolve_guild(self.cfg["guild_id"], await self._channel_guild("dev_channel_id"))
        if self.cfg["report_channel_id"] is not None:
            core.check_report_channel(self.guild_id, self.cfg["report_channel_id"],
                                      await self._channel_guild("report_channel_id"))
        guild = discord.Object(id=self.guild_id)
        self._add_commands(guild)
        synced = await self.tree.sync(guild=guild)
        log.info("commands synced to server %s: %s", self.guild_id, ", ".join(c.name for c in synced))

    async def _channel_guild(self, key):
        """The server id of a configured channel, or None if the bot cannot read it."""
        try:
            channel = await self.fetch_channel(self.cfg[key])
        except discord.HTTPException as e:
            log.error("cannot read the %s channel %s (%s)", key.split("_")[0], self.cfg[key], type(e).__name__)
            return None
        return getattr(channel, "guild_id", None) or getattr(getattr(channel, "guild", None), "id", None)

    async def on_ready(self):
        log.info("online as %s", self.user)

    async def on_command_error(self, interaction, error):
        # Never show a traceback or error text to the player; it goes to the log only.
        log.error("command failed", exc_info=error)
        try:
            if interaction.response.is_done():
                await interaction.followup.send(GENERIC_ERROR, ephemeral=True)
            else:
                await interaction.response.send_message(GENERIC_ERROR, ephemeral=True)
        except discord.HTTPException:
            pass

    def _add_commands(self, guild):
        @app_commands.command(name="bugreport", description="Send a bug report (and your logs zip) to the Squadala! team.")
        @app_commands.describe(
            description="What happened, and what you were doing (solo or co-op, host or guest).",
            logs="Optional: the zip from the kit's collect-logs (zip only, 25 MB max).",
            version="Optional: your kit version, e.g. 0.11.3.",
        )
        @app_commands.guild_only()
        async def report(
            interaction: discord.Interaction,
            description: app_commands.Range[str, 5, core.MAX_DESCRIPTION],
            logs: discord.Attachment = None,
            version: app_commands.Range[str, 1, 32] = None,
        ):
            await self.handle_report(interaction, description, logs, version)

        self.tree.add_command(report, guild=guild)

    async def refuse(self, interaction, text):
        if interaction.response.is_done():
            await interaction.followup.send(text, ephemeral=True)
        else:
            await interaction.response.send_message(text, ephemeral=True)

    async def handle_report(self, interaction, description, logs, version):
        user = interaction.user
        # Only the owner's server, even if the bot is ever added to another one.
        if not core.guild_allowed(interaction.guild_id, self.guild_id):
            log.warning("refused /bugreport from server %s (not ours)", interaction.guild_id)
            await self.refuse(interaction, "This bot only takes reports in the Squadala! server.")
            return
        allowed = self.cfg["report_channel_id"]
        if not core.channel_allowed(interaction.channel_id, allowed, self.cfg["dev_channel_id"]):
            await self.refuse(interaction, f"Please use /bugreport in <#{allowed}>.")
            return
        if logs is not None:
            problem = core.check_attachment_meta(logs.filename, logs.size)
            if problem:
                await self.refuse(interaction, problem)
                return

        # Rate limits: taken now, before any await, so reports sent at once cannot all pass.
        wait = self.user_limit.acquire(user.id)
        if wait > 0:
            await self.refuse(interaction,
                              f"You sent several reports recently; please try again in {int(wait // 60) + 1} min.")
            return
        if self.global_limit.acquire("all") > 0:
            self.user_limit.release(user.id)
            log.warning("global report limit reached; refused a report from %s", user.name)
            await self.refuse(interaction, "Many reports came in this hour; please try again later.")
            return
        ok = False
        try:
            ok = await self._accept(interaction, user, description, logs, version)
        finally:
            if not ok:
                self.user_limit.release(user.id)
                self.global_limit.release("all")

    async def _accept(self, interaction, user, description, logs, version):
        incoming = logs.size if logs is not None else 0
        if core.over_budget(await asyncio.to_thread(core.dir_size, self.logs_dir), incoming):
            log.warning("player-logs is over its %d MB budget; refused a report from %s",
                        core.LOGS_BUDGET_BYTES // (1024 * 1024), user.name)
            await self.refuse(interaction, "The report box is full right now; please try again later.")
            return False

        await interaction.response.defer(ephemeral=True, thinking=True)
        data = None
        if logs is not None:
            try:
                data = await logs.read()
            except discord.HTTPException as e:
                log.warning("download of the attachment failed (%s)", type(e).__name__)
                await self.refuse(interaction, "I could not download the zip; please try again.")
                return False
            problem = core.check_zip_bytes(data)
            if problem:
                await self.refuse(interaction, problem)
                return False

        text = core.clean_description(description)
        when = dt.datetime.now(dt.timezone.utc)
        username = user.name
        display = getattr(user, "display_name", None) or username
        try:
            async with self.lock:
                if core.over_budget(core.dir_size(self.logs_dir), len(data) if data is not None else 0):
                    await self.refuse(interaction, "The report box is full right now; please try again later.")
                    return False
                number, folder = core.save_report(self.logs_dir, self.tracker, when, username, display, version,
                                                  logs.filename if logs is not None else None, data, text)
        except OSError:
            log.exception("saving a report from %s failed", username)
            await self.refuse(interaction, GENERIC_ERROR)
            return False
        rel = f"player-logs/{folder.name}"
        log.info("report #%d from %s saved to %s (%s)", number, username, rel,
                 f"{len(data)} bytes" if data is not None else "no zip")

        await self.post_to_dev(number, when, user, display, version, text, data, rel)
        await interaction.followup.send(f"Thanks, report #{number} received.", ephemeral=True)
        return True

    async def post_to_dev(self, number, when, user, display, version, text, data, rel):
        try:
            channel = self.get_channel(self.cfg["dev_channel_id"]) or await self.fetch_channel(self.cfg["dev_channel_id"])
            # Player text goes in a code block: no links, images, mentions or formatting render.
            body = f"{core.UNTRUSTED_LABEL}:\n```text\n{core.no_backticks(text)}\n```"
            embed = discord.Embed(title=f"Bug report #{number}", description=body, timestamp=when,
                                  colour=discord.Colour.orange())
            shown = core.no_backticks(" ".join((display or "").split()))[:64]
            embed.add_field(name="Player", value=f"`{shown}` (@{core.clean_handle(user.name)})")
            embed.add_field(name="Kit version", value=core.clean_version(version) or "?")
            embed.add_field(name="Saved", value=f"`{rel}`", inline=False)
            files = []
            if data is not None:
                limit = getattr(getattr(channel, "guild", None), "filesize_limit", 10 * 1024 * 1024)
                if len(data) <= limit:
                    files.append(discord.File(io.BytesIO(data), filename=f"report-{number}-logs.zip"))
                else:
                    embed.add_field(name="Logs", value="Zip too big to re-post here; saved on the PC.", inline=False)
            else:
                embed.add_field(name="Logs", value="None attached.", inline=False)
            await channel.send(embed=embed, files=files)
        except discord.HTTPException as e:
            # The report is already saved; the dev post is a copy.
            log.error("posting report #%d to the dev channel failed (%s)", number, type(e).__name__)


def main():
    here = Path(__file__).resolve().parent
    default_cfg = here.parent.parent.parent / "coord-multiplayer" / "secrets" / "discord-bot.json"
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--config", default=os.environ.get("DISCORD_BOT_CONFIG", str(default_cfg)))
    args = ap.parse_args()
    try:
        cfg = core.load_config(args.config)
    except core.ConfigError as e:
        print(f"discord-bot: {e}", file=sys.stderr)
        return 2
    cfg["coord_dir"].joinpath("player-logs").mkdir(parents=True, exist_ok=True)
    setup_logging(cfg["coord_dir"] / "player-logs" / "discord-bot.log", cfg["token"])
    bot = ReportBot(cfg)
    try:
        bot.run(cfg["token"], log_handler=None)
    except discord.LoginFailure:
        log.error("Discord refused the token (check secrets/discord-bot.json)")
        return 2
    except core.StartupError as e:
        log.error("%s", e)
        return 2  # run-bot.ps1 does not restart on 2: fix the setup first
    return 0


if __name__ == "__main__":
    sys.exit(main())
