// The crash relay's rules (docs/problem-reports.md, "The relay"): one zip a POST, from a kit that sends reports, passed on
// to the #dev-reports Discord webhook. The webhook's URL is a Worker secret (DISCORD_WEBHOOK), never in git or in a kit.
// Everything here is plain JavaScript with no Cloudflare API, so tests/relay.test.js runs it under Node.
//
// What it takes: POST /report, Content-Type application/zip, X-Squadala-Kind (crash, unclean or problem),
// X-Squadala-Kit (the kit's version), a body of at most MAX_BYTES that is a zip (its first local header and its end
// record). Everything else is refused before Discord is called. Rate limits: PER_IP_PER_HOUR from one address and
// GLOBAL_PER_HOUR in all, counted in fixed one-hour windows (Limiter). An address is kept only as a salted hash, only for
// the window it was counted in: a new window has a new salt and drops the old hashes.

export const MAX_BYTES = 8 * 1024 * 1024;   // (Discord takes 10 MB a message without a boost)
export const PER_IP_PER_HOUR = 3;
export const GLOBAL_PER_HOUR = 30;
export const WINDOW_MS = 60 * 60 * 1000;
export const KINDS = { crash: 'crash', unclean: 'game ended without closing', problem: 'problem report' };
const VERSION_RX = /^v?\d{1,3}\.\d{1,3}(?:\.\d{1,3})?(?:[-+][0-9A-Za-z.-]{1,32})?$/;
const WEBHOOK_RX = /^https:\/\/(?:canary\.|ptb\.)?discord(?:app)?\.com\/api\/webhooks\/\d+\/[\w-]+$/;

export function reply(status, text, extra = {}) {
    return new Response(text + '\n', {
        status,
        headers: { 'Content-Type': 'text/plain; charset=utf-8', 'Cache-Control': 'no-store', ...extra },
    });
}

// The request line and headers: { kind, version } or { error: Response }.
export function checkRequest(request) {
    const url = new URL(request.url);
    if (url.pathname !== '/report') return { error: reply(404, 'not found') };
    if (request.method !== 'POST') return { error: reply(405, 'POST only', { Allow: 'POST' }) };
    const type = (request.headers.get('Content-Type') || '').split(';')[0].trim().toLowerCase();
    if (type !== 'application/zip') return { error: reply(415, 'a zip only (Content-Type: application/zip)') };
    const kind = (request.headers.get('X-Squadala-Kind') || '').trim().toLowerCase();
    if (!Object.hasOwn(KINDS, kind)) return { error: reply(400, 'X-Squadala-Kind: crash, unclean or problem') };
    const version = (request.headers.get('X-Squadala-Kit') || '').trim();
    if (!VERSION_RX.test(version)) return { error: reply(400, 'X-Squadala-Kit: the kit version (v0.11.4)') };
    const length = request.headers.get('Content-Length');
    if (length !== null && !(/^\d+$/.test(length) && Number(length) <= MAX_BYTES)) {
        return { error: reply(413, `at most ${MAX_BYTES} bytes`) };
    }
    return { kind, version };
}

// The body, read up to the cap: a Uint8Array, or null when it is larger (whatever Content-Length said).
export async function readCapped(request, cap = MAX_BYTES) {
    if (!request.body) return new Uint8Array(0);
    const reader = request.body.getReader();
    const parts = [];
    let size = 0;
    for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        size += value.byteLength;
        if (size > cap) {
            await reader.cancel().catch(() => {});
            return null;
        }
        parts.push(value);
    }
    const out = new Uint8Array(size);
    let at = 0;
    for (const p of parts) { out.set(p, at); at += p.byteLength; }
    return out;
}

// A zip: a local file header first (PK 3 4) and an end of central directory record (PK 5 6) in the last 22 + 65535
// bytes, whose comment ends the file. An empty zip (no file) is refused.
export function looksLikeZip(bytes) {
    if (!(bytes instanceof Uint8Array) || bytes.length < 30 + 22) return false;
    if (bytes[0] !== 0x50 || bytes[1] !== 0x4b || bytes[2] !== 0x03 || bytes[3] !== 0x04) return false;
    const lowest = Math.max(0, bytes.length - 22 - 0xffff);
    for (let i = bytes.length - 22; i >= lowest; i--) {
        if (bytes[i] === 0x50 && bytes[i + 1] === 0x4b && bytes[i + 2] === 0x05 && bytes[i + 3] === 0x06) {
            const comment = bytes[i + 20] | (bytes[i + 21] << 8);
            const entries = bytes[i + 10] | (bytes[i + 11] << 8);
            return i + 22 + comment === bytes.length && entries > 0;
        }
    }
    return false;
}

function hex(buffer) {
    return [...new Uint8Array(buffer)].map((b) => b.toString(16).padStart(2, '0')).join('');
}

// The rate limit, on a store with get/put (and setAlarm when there is one): the Durable Object's storage in the Worker,
// a Map-backed fake in the tests. One record: the window's start, its salt, the count in all and per address hash.
export class Limiter {
    constructor(storage, { now = () => Date.now(), salt = () => hex(crypto.getRandomValues(new Uint8Array(16))) } = {}) {
        this.storage = storage;
        this.now = now;
        this.salt = salt;
    }

    async take(ip) {
        const now = this.now();
        let w = await this.storage.get('window');
        if (!w || now - w.start >= WINDOW_MS || now < w.start) {
            w = { start: now, salt: this.salt(), total: 0, ips: {} };   // (the old window's hashes go with it)
            if (this.storage.setAlarm) await this.storage.setAlarm(now + WINDOW_MS);
        }
        const retryAfter = Math.max(1, Math.ceil((w.start + WINDOW_MS - now) / 1000));
        if (w.total >= GLOBAL_PER_HOUR) return { ok: false, retryAfter, why: 'all' };
        const digest = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(w.salt + '|' + String(ip || '')));
        const key = hex(digest).slice(0, 24);
        const count = w.ips[key] || 0;
        if (count >= PER_IP_PER_HOUR) {
            await this.storage.put('window', w);
            return { ok: false, retryAfter, why: 'address' };
        }
        w.ips[key] = count + 1;
        w.total += 1;
        await this.storage.put('window', w);
        return { ok: true };
    }
}

// The Discord message: one line naming the kind and the kit, the zip as its file; no one is mentioned.
export async function forward(webhook, bytes, kind, version, { fetchFn = fetch, now = () => new Date() } = {}) {
    const stamp = now().toISOString().replace(/[:.]/g, '-').slice(0, 19);
    const form = new FormData();
    form.append('payload_json', JSON.stringify({
        content: `Squadala! ${KINDS[kind]}, kit ${version}`,
        allowed_mentions: { parse: [] },
    }));
    form.append('files[0]', new Blob([bytes], { type: 'application/zip' }), `squadala-${kind}-${stamp}.zip`);
    const res = await fetchFn(webhook, { method: 'POST', body: form });
    return res.ok;
}

// One request: { limiter: { take(ip) }, fetchFn } are the Worker's (worker.js) or the tests'.
export async function handle(request, env, { limiter, fetchFn = fetch, now } = {}) {
    const checked = checkRequest(request);
    if (checked.error) return checked.error;
    const webhook = env && env.DISCORD_WEBHOOK;
    if (!webhook || !WEBHOOK_RX.test(webhook)) return reply(503, 'not set up');
    const limit = await limiter.take(request.headers.get('CF-Connecting-IP') || '');
    if (!limit.ok) return reply(429, 'too many reports, try later', { 'Retry-After': String(limit.retryAfter) });
    const bytes = await readCapped(request);
    if (bytes === null) return reply(413, `at most ${MAX_BYTES} bytes`);
    if (!looksLikeZip(bytes)) return reply(400, 'not a zip');
    let ok = false;
    try { ok = await forward(webhook, bytes, checked.kind, checked.version, { fetchFn, now }); } catch { ok = false; }
    return ok ? reply(202, 'sent') : reply(502, 'could not pass it on');
}
