/* SPDX-License-Identifier: GPL-3.0-or-later
   The mods list between the co-op module's Mods tab and Hookshot's mod loader (oot-remake-multiplayer, docs/mods.md).

   Hookshot indexes its mods folder once, at the start, before the module is loaded, and keeps the game's files open
   while it runs. So the tab never renames or moves a mod: it writes one small file into the mods folder, and Hookshot's
   host/mods.c (our patch 0011) reads it at the next start:

     <mods>/coop-mods.list      written by the module: the order and the switches
         coop-mods 1
         on <name>              first row = wins a file two mods have (it is indexed last)
         off <name>             left out
       <name> is the mod's name as Hookshot logs it: its path under the mods folder with '/'. A mod the list does not
       name is on and keeps Hookshot's own order (by name) under the named ones. Lines it does not know are skipped.

     <mods>/coop-mods.applied   written by Hookshot at each start: what this run indexed
         coop-mods-applied 1 <process id>
         on <files> <name>      in the order indexed (the last one wins)
         off <name>

   The same file is in Hookshot's host/ through our patch and in our repository (hookshot/src): keep them equal
   (tests/hookshot/mods_test.cpp checks it). Plain C, header only, no Hookshot or co-op include. */
#ifndef HOS_COOP_MODS_H
#define HOS_COOP_MODS_H
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define HOS_COOP_MODS_VERSION 1
#define HOS_COOP_MODS_LIST_FILE "coop-mods.list"
#define HOS_COOP_MODS_APPLIED_FILE "coop-mods.applied"
#define HOS_COOP_MODS_MAX 256    /* rows read; more are skipped */
#define HOS_COOP_MODS_NAME 260   /* a name with its 0 (MAX_PATH) */
/* The most a list file can hold that is read: a caller's buffer for it. */
#define HOS_COOP_MODS_TEXT_MAX (HOS_COOP_MODS_MAX * (HOS_COOP_MODS_NAME + 8) + 64)

typedef struct HosCoopModsRow {
    char name[HOS_COOP_MODS_NAME];
    int off;
} HosCoopModsRow;

typedef struct HosCoopModsList {
    int count;
    HosCoopModsRow rows[HOS_COOP_MODS_MAX];
} HosCoopModsList;

/* Names are compared as Hookshot sorts them: ASCII letters without case; '\' counts as '/'. */
static inline int hos_coop_mods_fold(int c) {
    if (c == '\\') return '/';
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}
static inline int hos_coop_mods_same(const char *a, const char *b) {
    for (;; a++, b++) {
        if (hos_coop_mods_fold((unsigned char)*a) != hos_coop_mods_fold((unsigned char)*b)) return 0;
        if (!*a) return 1;
    }
}

/* Reads a list file's text (n bytes; no 0 needed). 1 when it is a list of this version; else 0 and an empty list. */
static inline int hos_coop_mods_parse(const char *text, size_t n, HosCoopModsList *out) {
    size_t at = 0;
    int line_no = 0;
    out->count = 0;
    while (at < n) {
        size_t end = at, len;
        const char *line = text + at;
        while (end < n && text[end] != '\n') end++;
        len = end - at;
        at = end + 1;
        if (len && line[len - 1] == '\r') len--;
        if (line_no++ == 0) {
            if (len != 11 || memcmp(line, "coop-mods 1", 11)) return 0;
            continue;
        }
        {
            int off;
            int i, known = 0;
            if (len > 3 && !memcmp(line, "on ", 3)) off = 0, line += 3, len -= 3;
            else if (len > 4 && !memcmp(line, "off ", 4)) off = 1, line += 4, len -= 4;
            else continue;
            if (len >= HOS_COOP_MODS_NAME || out->count >= HOS_COOP_MODS_MAX) continue;
            memcpy(out->rows[out->count].name, line, len);
            out->rows[out->count].name[len] = 0;
            for (i = 0; i < out->count && !known; i++)   /* (a name twice: its first row counts) */
                known = hos_coop_mods_same(out->rows[i].name, out->rows[out->count].name);
            if (known) continue;
            out->rows[out->count++].off = off;
        }
    }
    return line_no > 0;
}

/* A mod's place: -1 = off; 0 = not in the list (Hookshot's own order, under the listed ones); else the higher wins
   (the list's first row has the highest). Mods are indexed by rising rank, then by name. */
static inline int hos_coop_mods_rank(const HosCoopModsList *list, const char *name) {
    int i;
    for (i = 0; i < list->count; i++)
        if (hos_coop_mods_same(list->rows[i].name, name)) return list->rows[i].off ? -1 : list->count - i;
    return 0;
}

/* The applied file's lines (each with its '\n'); what snprintf returns. */
static inline int hos_coop_mods_applied_head(char *out, size_t cap, unsigned long pid) {
    return snprintf(out, cap, "coop-mods-applied %d %lu\n", HOS_COOP_MODS_VERSION, pid);
}
static inline int hos_coop_mods_applied_row(char *out, size_t cap, const char *name, int files) {   /* files < 0: off */
    return files < 0 ? snprintf(out, cap, "off %s\n", name) : snprintf(out, cap, "on %d %s\n", files, name);
}

/* Co-op's own packs (the puppet pack, the HUD pack): the kit keeps them on and builds the game with them; never listed.
   A name whose last part starts with coop-puppet, coop_hud or coop-hud. */
static inline int hos_coop_mods_own(const char *name) {
    static const char *const own[] = {"coop-puppet", "coop_hud", "coop-hud"};
    const char *leaf = name, *p;
    size_t i, k;
    for (p = name; *p; p++)
        if (*p == '/' || *p == '\\') leaf = p + 1;
    for (i = 0; i < sizeof own / sizeof own[0]; i++) {
        for (k = 0; own[i][k] && hos_coop_mods_fold((unsigned char)leaf[k]) == own[i][k]; k++) {}
        if (!own[i][k]) return 1;
    }
    return 0;
}

/* A mod's code against the game's build (player report: a code mod switched in the Mods tab, then the game started from
   longshot.exe, which does not build). The game is compiled ahead of time, and a mod's ExeFS patches go into it only when
   it is built (the kit's Start Squadala builds again when they changed). The build lists what it took in
   <game>/exefs_patches.txt (nx2/exefs_patches.py): "<mod>\t<file>\t<hash>\t<kind>[\t<note>]" a line, '#' a comment,
   <kind> code or data (applied), other, empty or bad (not applied). The build lists only the mods that were on, and writes
   no list when no mod had a patch file (nx2/make_game.py): no list reads as an empty one (n = 0). */
#define HOS_COOP_MODS_BUILT_NONE 0   /* the list names none of its files: the game was built while it was off (or gone) */
#define HOS_COOP_MODS_BUILT_CODE 1   /* code or data of it applied: its code is in the game */
#define HOS_COOP_MODS_BUILT_NAMED 2  /* named, nothing applied (another game version's, empty): its code changes nothing */
static inline int hos_coop_mods_built(const char *list, size_t n, const char *name) {
    size_t at = 0;
    int named = 0;
    while (at < n) {
        size_t end = at, len, f = 0, field = 0;
        const char *line = list + at;
        while (end < n && list[end] != '\n') end++;
        len = end - at;
        at = end + 1;
        if (!len || line[0] == '#') continue;
        while (f < len && line[f] != '\t') f++;
        if (f == len || strlen(name) != f) continue;
        {
            size_t k;
            int same = 1;
            for (k = 0; k < f && same; k++)
                same = hos_coop_mods_fold((unsigned char)line[k]) == hos_coop_mods_fold((unsigned char)name[k]);
            if (!same) continue;
        }
        named = 1;
        for (f = 0; f < len && field < 3; f++)   /* (the fourth field: the kind) */
            if (line[f] == '\t') field++;
        if (field == 3 && f + 4 <= len && (!memcmp(line + f, "code", 4) || !memcmp(line + f, "data", 4)))
            return HOS_COOP_MODS_BUILT_CODE;
    }
    return named ? HOS_COOP_MODS_BUILT_NAMED : HOS_COOP_MODS_BUILT_NONE;
}

/* Whether a mod's files are used this start: as the game was built for it, not as switched since. A mod whose code is in
   the game keeps its files on (its code reads them); a mod with ExeFS patch files the build did not take keeps its files
   off (its files without its code: Character Select's picks do nothing, and its tables lack what the kit's start merges
   in). The next game build (Start Squadala) takes the switch, code and files together. Co-op's own packs: as switched.
   on: the switch; built: hos_coop_mods_built; patch_files: its .ips and .pchtxt files. */
static inline int hos_coop_mods_as_built(int on, int built, int patch_files, const char *name) {
    if (hos_coop_mods_own(name)) return on;
    if (built == HOS_COOP_MODS_BUILT_CODE) return 1;
    if (built == HOS_COOP_MODS_BUILT_NONE && patch_files > 0) return 0;
    return on;
}

#endif
