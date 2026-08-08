/* fat_dir.c — FAT32 directories: entries, long names, and the slot allocator.
 *
 * This is where FAT's age shows. A directory is an array of 32-byte records;
 * a name longer than 8.3 is stored as a run of "long file name" records
 * immediately before the real one, each holding 13 UCS-2 code units, in
 * reverse order, tied to the real record by a one-byte checksum of its short
 * name. All of that is implemented here and nowhere else.
 *
 * TWO RULES THIS FILE ENFORCES
 *
 *   1. A NAME THIS FILESYSTEM REPORTS CAN ALWAYS BE LOOKED UP AGAIN. If a long
 *      name does not fit VFS_NAME_MAX, or is not representable as UTF-8, the
 *      entry is reported under its 8.3 short name instead of being hidden or
 *      truncated. A truncated name would name a different file; a hidden entry
 *      would be a file the operator can see from their Mac and this machine
 *      swears does not exist.
 *
 *   2. A WHOLE ENTRY SET IS WRITTEN IN ONE SECTOR WRITE. fat_dir_add() only
 *      ever places a set (long-name records + the short record) inside a
 *      single 512-byte sector, so creating a file is atomic with respect to a
 *      crash: the file is there with its full name, or it is not there. With
 *      VFS_NAME_MAX at 63 a set is at most 6 records of the 16 in a sector, so
 *      this costs nothing but a slightly pickier free-slot search.
 */

#include "fat.h"
#include "kernel.h"
#include "rtc.h"
#include <string.h>

#define ENTS_PER_SECTOR (BLOCK_SECTOR_SIZE / FAT_DIRENT_SIZE)   /* 16 */
#define LFN_CHARS       13
/* A FAT directory may hold at most 65536 entries. The cap also bounds every
 * scan in this file, so a corrupt chain cannot turn a lookup into a hang. */
#define FAT_MAX_DIR_ENTRIES 65536u

/* Short-entry field offsets. */
#define DE_NAME      0
#define DE_ATTR      11
#define DE_NTRES     12
#define DE_CRT_TIME  14
#define DE_CRT_DATE  16
#define DE_ACC_DATE  18
#define DE_CLUS_HI   20
#define DE_WRT_TIME  22
#define DE_WRT_DATE  24
#define DE_CLUS_LO   26
#define DE_SIZE      28

/* ====================================================================== */
/* 1. time                                                                */
/* ====================================================================== */

void fat_now(uint16_t *date, uint16_t *time) {
    rtc_time_t t;
    /* No clock (host tests, or a machine whose CMOS is dead) means a fixed,
     * legal FAT epoch rather than a plausible-looking lie. */
    if (rtc_read(&t) != 0 || t.year < 1980 || t.year > 2107) {
        *date = (uint16_t)((0 << 9) | (1 << 5) | 1);   /* 1980-01-01 */
        *time = 0;
        return;
    }
    *date = (uint16_t)(((t.year - 1980) << 9) | (t.month << 5) | t.day);
    *time = (uint16_t)((t.hour << 11) | (t.minute << 5) | (t.second / 2));
}

/* ====================================================================== */
/* 2. character conversion                                                */
/* ====================================================================== */

/* UTF-8 -> UCS-2, strict. Returns the number of code units, or -1 for
 * malformed input, an overlong encoding, a surrogate, or anything outside the
 * Basic Multilingual Plane (which UCS-2, and therefore FAT, cannot hold).
 * Strict because these bytes come from model output. */
static int utf8_to_ucs2(const char *s, uint16_t *out, int cap) {
    int n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t cp;
        if (p[0] < 0x80) { cp = p[0]; p += 1; }
        else if ((p[0] & 0xE0) == 0xC0) {
            if ((p[1] & 0xC0) != 0x80) return -1;
            cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
            if (cp < 0x80) return -1;                  /* overlong */
            p += 2;
        } else if ((p[0] & 0xF0) == 0xE0) {
            if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) return -1;
            cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6)
               | (p[2] & 0x3F);
            if (cp < 0x800) return -1;                 /* overlong */
            if (cp >= 0xD800 && cp <= 0xDFFF) return -1; /* surrogate */
            p += 3;
        } else {
            return -1;                                  /* 4-byte or invalid */
        }
        if (n >= cap) return -1;
        out[n++] = (uint16_t)cp;
    }
    return n;
}

/* UCS-2 -> UTF-8. Returns bytes written (NUL terminated), or -1 if it does not
 * fit in `cap` or holds an unpaired surrogate. */
static int ucs2_to_utf8(const uint16_t *src, int n, char *dst, size_t cap) {
    size_t o = 0;
    for (int i = 0; i < n; i++) {
        uint32_t cp = src[i];
        if (cp >= 0xD800 && cp <= 0xDFFF) return -1;
        if (cp < 0x80) {
            if (o + 1 >= cap) return -1;
            dst[o++] = (char)cp;
        } else if (cp < 0x800) {
            if (o + 2 >= cap) return -1;
            dst[o++] = (char)(0xC0 | (cp >> 6));
            dst[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            if (o + 3 >= cap) return -1;
            dst[o++] = (char)(0xE0 | (cp >> 12));
            dst[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            dst[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    dst[o] = '\0';
    return (int)o;
}

static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

/* Case-insensitive over ASCII only, which is what FAT itself does for the
 * characters this filesystem lets into a name. */
static int name_eq_ci(const char *a, const char *b) {
    while (*a && *b) { if (up(*a) != up(*b)) return 0; a++; b++; }
    return *a == '\0' && *b == '\0';
}

/* ====================================================================== */
/* 3. short names                                                         */
/* ====================================================================== */

uint8_t fat_short_checksum(const uint8_t name[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + name[i]);
    return sum;
}

/* Render a directory entry's 8.3 name as "NAME.EXT".
 *
 * The two bits in the reserved byte at offset 12 are not decoration. Linux and
 * macOS store an all-lowercase 8.3 name WITHOUT long-name records and set them
 * instead, so a reader that ignores them reports the operator's "drivers"
 * directory as "DRIVERS" — a name that is still findable (lookup is
 * case-insensitive) but is not the one they typed, and not the one they will
 * see again when they mount the image back on their Mac. */
static void short_to_string(const uint8_t *e, char *out) {
    int lower_base = (e[DE_NTRES] & 0x08) != 0;
    int lower_ext  = (e[DE_NTRES] & 0x10) != 0;
    int o = 0;
    for (int i = 0; i < 8; i++) {
        if (e[i] == ' ') break;
        char c = (char)(i == 0 && e[i] == FAT_KANJI_E5 ? FAT_DELETED : e[i]);
        if (lower_base && c >= 'A' && c <= 'Z') c = (char)(c + 32);
        out[o++] = c;
    }
    if (e[8] != ' ') {
        out[o++] = '.';
        for (int i = 8; i < 11; i++) {
            if (e[i] == ' ') break;
            char c = (char)e[i];
            if (lower_ext && c >= 'A' && c <= 'Z') c = (char)(c + 32);
            out[o++] = c;
        }
    }
    out[o] = '\0';
}

static int short_char_ok(char c) {
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    return strchr("$%'-_@~`!(){}^#&", c) != NULL && c != '\0';
}

/* Build the 8.3 form of `name`. Sets *needs_lfn when the result is not an
 * exact, reversible rendering of the name the caller asked for — which is the
 * only case in which long-name records have to be written. */
static void short_name_build(const char *name, uint8_t out[11], int *needs_lfn) {
    memset(out, ' ', 11);
    *needs_lfn = 0;

    /* Leading dots and spaces cannot be represented at all. */
    const char *p = name;
    while (*p == '.' || *p == ' ') { p++; *needs_lfn = 1; }

    /* The extension is what follows the LAST dot. */
    const char *dot = NULL;
    for (const char *q = p; *q; q++) if (*q == '.') dot = q;
    size_t base_len = dot ? (size_t)(dot - p) : strlen(p);
    const char *ext = dot ? dot + 1 : NULL;

    /* More than one dot, or a dot with nothing after it, is not 8.3. */
    for (const char *q = p; q < p + base_len; q++) if (*q == '.') *needs_lfn = 1;
    if (dot && !*ext) *needs_lfn = 1;

    int o = 0;
    for (size_t i = 0; i < base_len; i++) {
        char c = p[i];
        if (c >= 'a' && c <= 'z') { c = up(c); *needs_lfn = 1; }
        if (!short_char_ok(c)) { c = '_'; *needs_lfn = 1; }
        if (o < 8) out[o++] = (uint8_t)c;
        else *needs_lfn = 1;
    }
    if (o == 0) { out[0] = '_'; *needs_lfn = 1; }

    if (ext) {
        int e = 0;
        for (const char *q = ext; *q; q++) {
            char c = *q;
            if (c >= 'a' && c <= 'z') { c = up(c); *needs_lfn = 1; }
            if (!short_char_ok(c)) { c = '_'; *needs_lfn = 1; }
            if (e < 3) out[8 + e++] = (uint8_t)c;
            else *needs_lfn = 1;
        }
    }
    /* 0xE5 as the first byte means "deleted", so a real name starting with it
     * is stored as 0x05. */
    if (out[0] == FAT_DELETED) out[0] = FAT_KANJI_E5;
}

/* ====================================================================== */
/* 4. reading a directory                                                 */
/* ====================================================================== */

void fat_dircur_init(fat_dircur_t *c, uint32_t dir_clus) {
    memset(c, 0, sizeof *c);
    c->dir_clus = dir_clus;
}

/* Map an entry index to its sector and offset. 1 = located, 0 = past the end
 * of the chain, <0 = error. */
static int locate(fat_vol_t *v, uint32_t dir_clus, uint32_t index,
                  uint32_t *sec, uint32_t *off) {
    uint32_t epc = v->bytes_per_clus / FAT_DIRENT_SIZE;
    uint32_t pos = index / epc, within = index % epc;
    uint32_t c = dir_clus;
    for (uint32_t i = 0; i < pos; i++) {
        uint32_t nx;
        int rc = fat_chain_next(v, c, &nx);
        if (rc != VFS_OK) return rc;
        if (nx == 0) return 0;
        c = nx;
    }
    if (!fat_clus_valid(v, c)) return VFS_EINVAL;
    uint32_t byte = within * FAT_DIRENT_SIZE;
    *sec = fat_clus_sector(v, c) + byte / BLOCK_SECTOR_SIZE;
    *off = byte % BLOCK_SECTOR_SIZE;
    return 1;
}

static void lfn_reset(fat_dircur_t *c) {
    c->lfn_have = 0; c->lfn_count = 0; c->lfn_sum = 0; c->lfn_first = 0;
}

/* Pull the 13 UCS-2 units out of one long-name record. */
static void lfn_extract(const uint8_t *e, uint16_t *dst) {
    static const int off[LFN_CHARS] = { 1,3,5,7,9, 14,16,18,20,22,24, 28,30 };
    for (int i = 0; i < LFN_CHARS; i++) dst[i] = fat_rd16(e + off[i]);
}

int fat_dir_next(fat_vol_t *v, fat_dircur_t *c, fat_dirent_t *out) {
    uint32_t epc = v->bytes_per_clus / FAT_DIRENT_SIZE;

    for (;;) {
        if (c->index >= FAT_MAX_DIR_ENTRIES) return 0;

        /* Advance the cluster cursor to the one holding c->index. want_pos is
         * monotonic, so this loop runs once per cluster boundary crossed, not
         * once per entry. */
        uint32_t want_pos = c->index / epc;
        if (c->clus == 0) { c->clus = c->dir_clus; c->clus_index = 0; }
        while (c->clus_index < want_pos) {
            uint32_t nx;
            int rc = fat_chain_next(v, c->clus, &nx);
            if (rc != VFS_OK) return rc;
            if (nx == 0) return 0;                 /* end of the directory */
            c->clus = nx;
            c->clus_index++;
        }
        if (!fat_clus_valid(v, c->clus)) return VFS_EINVAL;

        uint32_t within = c->index % epc;
        uint32_t byte   = within * FAT_DIRENT_SIZE;
        uint32_t sec    = fat_clus_sector(v, c->clus) + byte / BLOCK_SECTOR_SIZE;
        uint32_t off    = byte % BLOCK_SECTOR_SIZE;

        const uint8_t *p;
        int rc = fat_sector_read(v, sec, &p);
        if (rc != VFS_OK) return rc;

        /* Copy out before anything else can touch the sector cache. */
        uint8_t e[FAT_DIRENT_SIZE];
        memcpy(e, p + off, FAT_DIRENT_SIZE);
        uint32_t this_index = c->index;
        c->index++;

        if (e[0] == 0x00) return 0;                /* no entries beyond here */
        if (e[0] == FAT_DELETED) { lfn_reset(c); continue; }

        if (e[DE_ATTR] == FAT_ATTR_LFN) {
            uint8_t ord = e[0];
            int last = (ord & 0x40) != 0;
            int seq  = ord & 0x3F;
            /* 20 records x 13 chars is the 255-character maximum. */
            if (seq < 1 || seq > 20) { lfn_reset(c); continue; }
            if (last) {
                lfn_reset(c);
                c->lfn_count = seq;
                c->lfn_sum   = e[13];
                c->lfn_first = this_index;
                memset(c->lfn, 0, sizeof c->lfn);
            }
            /* Records must arrive in descending order, all with one checksum.
             * Anything else is an orphaned or torn set: drop it and fall back
             * to the short name rather than assembling a name that was never
             * written. */
            if (c->lfn_count == 0 || e[13] != c->lfn_sum ||
                seq != c->lfn_count - c->lfn_have) {
                lfn_reset(c);
                continue;
            }
            lfn_extract(e, c->lfn + (seq - 1) * LFN_CHARS);
            c->lfn_have++;
            continue;
        }

        if (e[DE_ATTR] & FAT_ATTR_VOLUME_ID) { lfn_reset(c); continue; }

        /* A real entry. */
        memset(out, 0, sizeof *out);
        memcpy(out->raw, e, FAT_DIRENT_SIZE);
        out->index       = this_index;
        out->first_index = this_index;
        out->attr        = e[DE_ATTR];
        out->size        = fat_rd32(e + DE_SIZE);
        out->clus        = ((uint32_t)fat_rd16(e + DE_CLUS_HI) << 16)
                         | fat_rd16(e + DE_CLUS_LO);

        int named = 0;
        if (c->lfn_count > 0 && c->lfn_have == c->lfn_count &&
            c->lfn_sum == fat_short_checksum(e)) {
            int n = c->lfn_count * LFN_CHARS;
            for (int i = 0; i < n; i++) if (c->lfn[i] == 0) { n = i; break; }
            if (ucs2_to_utf8(c->lfn, n, out->name, sizeof out->name) > 0) {
                out->first_index = c->lfn_first;
                named = 1;
            }
            /* If it did not fit VFS_NAME_MAX, fall through to the short name:
             * see rule 1 at the top of this file. */
        }
        if (!named) short_to_string(e, out->name);
        lfn_reset(c);
        return 1;
    }
}

int fat_dir_find(fat_vol_t *v, uint32_t dir_clus, const char *name,
                 fat_dirent_t *out) {
    fat_dircur_t cur;
    fat_dircur_init(&cur, dir_clus);
    for (;;) {
        int rc = fat_dir_next(v, &cur, out);
        if (rc < 0) return rc;
        if (rc == 0) return VFS_ENOENT;
        if (name_eq_ci(out->name, name)) return VFS_OK;
    }
}

int fat_dir_is_empty(fat_vol_t *v, uint32_t dir_clus) {
    fat_dircur_t cur;
    fat_dirent_t de;
    fat_dircur_init(&cur, dir_clus);
    for (;;) {
        int rc = fat_dir_next(v, &cur, &de);
        if (rc < 0) return rc;
        if (rc == 0) return 1;
        if (strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0) continue;
        return 0;
    }
}

/* ====================================================================== */
/* 5. writing a directory                                                 */
/* ====================================================================== */

int fat_dir_init_cluster(fat_vol_t *v, uint32_t clus, uint32_t parent_clus) {
    uint8_t sec[BLOCK_SECTOR_SIZE];
    memset(sec, 0, sizeof sec);
    uint16_t date, time;
    fat_now(&date, &time);

    memset(sec, ' ', 11);
    sec[0] = '.';
    sec[DE_ATTR] = FAT_ATTR_DIRECTORY;
    fat_wr16(sec + DE_CLUS_HI, (uint16_t)(clus >> 16));
    fat_wr16(sec + DE_CLUS_LO, (uint16_t)(clus & 0xFFFF));
    fat_wr16(sec + DE_WRT_TIME, time);
    fat_wr16(sec + DE_WRT_DATE, date);
    fat_wr16(sec + DE_CRT_TIME, time);
    fat_wr16(sec + DE_CRT_DATE, date);

    uint8_t *dd = sec + FAT_DIRENT_SIZE;
    memset(dd, ' ', 11);
    dd[0] = '.'; dd[1] = '.';
    dd[DE_ATTR] = FAT_ATTR_DIRECTORY;
    /* ".." of a child of the root is written as cluster 0, not as the root's
     * real cluster number. That is what the specification says and what other
     * implementations check for. */
    uint32_t pc = (parent_clus == v->root_clus) ? 0 : parent_clus;
    fat_wr16(dd + DE_CLUS_HI, (uint16_t)(pc >> 16));
    fat_wr16(dd + DE_CLUS_LO, (uint16_t)(pc & 0xFFFF));
    fat_wr16(dd + DE_WRT_TIME, time);
    fat_wr16(dd + DE_WRT_DATE, date);
    fat_wr16(dd + DE_CRT_TIME, time);
    fat_wr16(dd + DE_CRT_DATE, date);

    return fat_sector_write(v, fat_clus_sector(v, clus), sec);
}

/* Append one zeroed cluster to a directory. */
static int dir_extend(fat_vol_t *v, uint32_t dir_clus, uint32_t *out_clus) {
    uint32_t last = dir_clus, guard = 0;
    for (;;) {
        uint32_t nx;
        int rc = fat_chain_next(v, last, &nx);
        if (rc != VFS_OK) return rc;
        if (nx == 0) break;
        last = nx;
        if (++guard > v->clusters) return VFS_EINVAL;
    }
    uint32_t nc;
    int rc = fat_alloc_cluster(v, &nc);          /* zeroed, marked end        */
    if (rc != VFS_OK) return rc;
    rc = fat_barrier(v);                         /* durable before it is seen */
    if (rc != VFS_OK) return rc;
    rc = fat_set(v, last, nc);                   /* now it is part of the dir */
    if (rc != VFS_OK) return rc;
    *out_clus = nc;
    return VFS_OK;
}

/* Is this 11-byte short name already used in the directory? */
static int short_name_taken(fat_vol_t *v, uint32_t dir_clus, const uint8_t nm[11]) {
    uint32_t epc = v->bytes_per_clus / FAT_DIRENT_SIZE;
    uint32_t clus = dir_clus, index = 0;
    while (fat_clus_valid(v, clus) && index < FAT_MAX_DIR_ENTRIES) {
        for (uint32_t i = 0; i < epc; i++, index++) {
            uint32_t byte = i * FAT_DIRENT_SIZE;
            uint32_t sec  = fat_clus_sector(v, clus) + byte / BLOCK_SECTOR_SIZE;
            const uint8_t *p;
            if (fat_sector_read(v, sec, &p) != VFS_OK) return 1;  /* assume taken */
            const uint8_t *e = p + byte % BLOCK_SECTOR_SIZE;
            if (e[0] == 0x00) return 0;
            if (e[0] == FAT_DELETED) continue;
            if (e[DE_ATTR] == FAT_ATTR_LFN) continue;
            if (memcmp(e, nm, 11) == 0) return 1;
        }
        uint32_t nx;
        if (fat_chain_next(v, clus, &nx) != VFS_OK) return 1;
        if (nx == 0) return 0;
        clus = nx;
    }
    return 0;
}

/* Pick a short name that no entry in this directory already uses.
 *
 * "NAME~1" up to "NAME~4" first, because that is what everything else
 * produces and what an operator expects to see. After four collisions, switch
 * to the hashed form Windows uses ("NA1F3C~1"): a directory holding a hundred
 * files whose names share a prefix — which is exactly what a machine that
 * writes its own files produces — must not turn creation into a linear search
 * that eventually fails. */
static int short_alias_pick(fat_vol_t *v, uint32_t dir_clus, const char *longname,
                            uint8_t nm[11]) {
    uint8_t base[11];
    memcpy(base, nm, 11);

    for (int n = 1; n <= 4; n++) {
        memcpy(nm, base, 11);
        int cut = 6;
        while (cut > 0 && base[cut - 1] == ' ') cut--;
        nm[cut]     = '~';
        nm[cut + 1] = (uint8_t)('0' + n);
        for (int i = cut + 2; i < 8; i++) nm[i] = ' ';
        if (!short_name_taken(v, dir_clus, nm)) return VFS_OK;
    }

    uint32_t h = 0;
    for (const char *p = longname; *p; p++) h = h * 31u + (unsigned char)*p;
    static const char hex[] = "0123456789ABCDEF";
    for (uint32_t i = 0; i < 4096; i++) {
        uint32_t x = (h + i * 2654435761u) & 0xFFFF;
        memcpy(nm, base, 11);
        if (nm[0] == ' ') nm[0] = '_';
        if (nm[1] == ' ') nm[1] = '_';
        nm[2] = (uint8_t)hex[(x >> 12) & 0xF];
        nm[3] = (uint8_t)hex[(x >> 8) & 0xF];
        nm[4] = (uint8_t)hex[(x >> 4) & 0xF];
        nm[5] = (uint8_t)hex[x & 0xF];
        nm[6] = '~';
        nm[7] = '1';
        if (!short_name_taken(v, dir_clus, nm)) return VFS_OK;
    }
    return VFS_EEXIST;
}

int fat_name_ok(const char *name) {
    if (!name || !name[0]) return 0;
    size_t len = strlen(name);
    if (len > VFS_NAME_MAX) return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
    /* FAT strips trailing dots and spaces, so a name ending in one would come
     * back as a different name and never be findable again. */
    if (name[len - 1] == '.' || name[len - 1] == ' ') return 0;
    for (const char *p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20) return 0;
        if (strchr("/\\:*?\"<>|", (char)c)) return 0;
    }
    uint16_t tmp[VFS_NAME_MAX + 1];
    if (utf8_to_ucs2(name, tmp, VFS_NAME_MAX + 1) < 0) return 0;
    return 1;
}

int fat_dir_add(fat_vol_t *v, uint32_t dir_clus, const char *name, uint8_t attr,
                uint32_t first_clus, uint32_t size, fat_dirent_t *out) {
    if (!fat_name_ok(name)) return VFS_EINVAL;
    if (v->read_only) return VFS_EINVAL;

    uint16_t ucs[VFS_NAME_MAX + 1];
    int nchars = utf8_to_ucs2(name, ucs, VFS_NAME_MAX + 1);
    if (nchars < 0) return VFS_EINVAL;

    uint8_t sname[11];
    int needs_lfn = 0;
    short_name_build(name, sname, &needs_lfn);
    if (needs_lfn) {
        int rc = short_alias_pick(v, dir_clus, name, sname);
        if (rc != VFS_OK) return rc;
    } else if (short_name_taken(v, dir_clus, sname)) {
        return VFS_EEXIST;
    }

    int nlfn = needs_lfn ? (nchars + LFN_CHARS - 1) / LFN_CHARS : 0;
    if (nlfn == 0 && needs_lfn) nlfn = 1;         /* an empty name cannot happen */
    uint32_t total = (uint32_t)nlfn + 1;
    /* Rule 2 at the top of this file. VFS_NAME_MAX = 63 gives at most 5
     * long-name records, so a set always fits inside one 16-entry sector. */
    if (total > ENTS_PER_SECTOR) return VFS_EINVAL;

    /* Find `total` consecutive free slots inside ONE sector.
     *
     * THE 0x00 TRAP. A first byte of 0x00 does not merely mean "this slot is
     * free": it means "there are no entries anywhere after this one", and
     * every FAT reader in the world stops there. So a set placed past a 0x00
     * slot that was skipped over is INVISIBLE — the file is written, the write
     * is reported as successful, and it cannot be opened again. (It cost this
     * filesystem exactly one round of that before the test caught it.) When a
     * sector holds the end-of-directory marker and the set does not fit after
     * it, the leftover slots are therefore retired to 0xE5 ("deleted", i.e.
     * free but not a terminator) before moving on. */
    uint32_t epc  = v->bytes_per_clus / FAT_DIRENT_SIZE;
    uint32_t clus = dir_clus;
    uint32_t base_index = 0;
    uint32_t found_index = FAT_NO_ENTRY;
    uint32_t found_sec = 0;
    uint8_t  sec[BLOCK_SECTOR_SIZE];

    while (found_index == FAT_NO_ENTRY) {
        if (!fat_clus_valid(v, clus)) return VFS_EINVAL;
        if (base_index >= FAT_MAX_DIR_ENTRIES) return VFS_ENOSPC;

        for (uint32_t s = 0; s < v->sec_per_clus; s++) {
            uint32_t sn = fat_clus_sector(v, clus) + s;
            int rc = fat_sector_read_into(v, sn, sec);
            if (rc != VFS_OK) return rc;

            uint32_t run = 0;
            int first_zero = -1;
            for (uint32_t i = 0; i < ENTS_PER_SECTOR; i++) {
                uint8_t f = sec[i * FAT_DIRENT_SIZE];
                if (f == 0x00 && first_zero < 0) first_zero = (int)i;
                if (f == 0x00 || f == FAT_DELETED) {
                    if (++run == total) {
                        found_index = base_index + s * ENTS_PER_SECTOR
                                    + (i + 1 - total);
                        found_sec   = sn;
                        break;
                    }
                } else {
                    run = 0;
                }
            }
            if (found_index != FAT_NO_ENTRY) break;

            if (first_zero >= 0) {
                for (uint32_t i = (uint32_t)first_zero; i < ENTS_PER_SECTOR; i++)
                    sec[i * FAT_DIRENT_SIZE] = FAT_DELETED;
                rc = fat_sector_write(v, sn, sec);
                if (rc != VFS_OK) return rc;
            }
        }
        if (found_index != FAT_NO_ENTRY) break;

        base_index += epc;
        uint32_t nx;
        int rc = fat_chain_next(v, clus, &nx);
        if (rc != VFS_OK) return rc;
        if (nx == 0) {
            rc = dir_extend(v, dir_clus, &nx);
            if (rc != VFS_OK) return rc;
        }
        clus = nx;
    }

    /* Build the whole set in a copy of the sector and write it once. */
    int rc = fat_sector_read_into(v, found_sec, sec);
    if (rc != VFS_OK) return rc;

    uint32_t slot0 = found_index % ENTS_PER_SECTOR;
    uint8_t  sum   = fat_short_checksum(sname);
    for (int i = 0; i < nlfn; i++) {
        /* Records are stored in reverse: the LAST piece of the name comes
         * first on disk. */
        int seq = nlfn - i;
        uint8_t *e = sec + (slot0 + (uint32_t)i) * FAT_DIRENT_SIZE;
        memset(e, 0, FAT_DIRENT_SIZE);
        e[0] = (uint8_t)(seq | (i == 0 ? 0x40 : 0));
        e[DE_ATTR] = FAT_ATTR_LFN;
        e[13] = sum;
        static const int off[LFN_CHARS] = { 1,3,5,7,9, 14,16,18,20,22,24, 28,30 };
        for (int k = 0; k < LFN_CHARS; k++) {
            int ci = (seq - 1) * LFN_CHARS + k;
            uint16_t ch;
            if (ci < nchars)       ch = ucs[ci];
            else if (ci == nchars) ch = 0x0000;    /* terminator */
            else                   ch = 0xFFFF;    /* padding    */
            fat_wr16(e + off[k], ch);
        }
    }

    uint8_t *e = sec + (slot0 + (uint32_t)nlfn) * FAT_DIRENT_SIZE;
    memset(e, 0, FAT_DIRENT_SIZE);
    memcpy(e, sname, 11);
    e[DE_ATTR] = attr;
    uint16_t date, time;
    fat_now(&date, &time);
    fat_wr16(e + DE_CRT_TIME, time);
    fat_wr16(e + DE_CRT_DATE, date);
    fat_wr16(e + DE_ACC_DATE, date);
    fat_wr16(e + DE_WRT_TIME, time);
    fat_wr16(e + DE_WRT_DATE, date);
    fat_wr16(e + DE_CLUS_HI, (uint16_t)(first_clus >> 16));
    fat_wr16(e + DE_CLUS_LO, (uint16_t)(first_clus & 0xFFFF));
    fat_wr32(e + DE_SIZE, size);

    rc = fat_sector_write(v, found_sec, sec);
    if (rc != VFS_OK) return rc;

    if (out) {
        memset(out, 0, sizeof *out);
        memcpy(out->raw, e, FAT_DIRENT_SIZE);
        strncpy(out->name, name, VFS_NAME_MAX);
        out->index       = found_index + (uint32_t)nlfn;
        out->first_index = found_index;
        out->clus        = first_clus;
        out->size        = size;
        out->attr        = attr;
    }
    return VFS_OK;
}

int fat_dir_remove(fat_vol_t *v, uint32_t dir_clus, const fat_dirent_t *de) {
    if (v->read_only) return VFS_EINVAL;
    /* The SHORT record goes first. It is what makes the file exist; the
     * long-name records in front of it are inert on their own and every
     * scanner (this one included) discards an orphaned set. A crash between
     * the two therefore leaves the file gone, not half-named. */
    uint8_t sec[BLOCK_SECTOR_SIZE];
    for (uint32_t i = de->index + 1; i-- > de->first_index; ) {
        uint32_t s, off;
        int rc = locate(v, dir_clus, i, &s, &off);
        if (rc < 0) return rc;
        if (rc == 0) break;
        rc = fat_sector_read_into(v, s, sec);
        if (rc != VFS_OK) return rc;
        sec[off] = FAT_DELETED;
        rc = fat_sector_write(v, s, sec);
        if (rc != VFS_OK) return rc;
        if (i == de->index) {
            /* Publish the deletion before touching anything else. */
            rc = fat_barrier(v);
            if (rc != VFS_OK) return rc;
        }
    }
    return VFS_OK;
}

int fat_dir_update(fat_vol_t *v, uint32_t dir_clus, uint32_t index,
                   uint32_t first_clus, uint32_t size) {
    if (v->read_only) return VFS_EINVAL;
    if (index == FAT_NO_ENTRY) return VFS_OK;     /* the root has no entry */
    uint32_t s, off;
    int rc = locate(v, dir_clus, index, &s, &off);
    if (rc < 0) return rc;
    if (rc == 0) return VFS_ENOENT;

    uint8_t sec[BLOCK_SECTOR_SIZE];
    rc = fat_sector_read_into(v, s, sec);
    if (rc != VFS_OK) return rc;
    uint8_t *e = sec + off;
    if (e[0] == 0x00 || e[0] == FAT_DELETED || e[DE_ATTR] == FAT_ATTR_LFN)
        return VFS_ENOENT;                        /* it went away underneath us */

    uint16_t date, time;
    fat_now(&date, &time);
    fat_wr16(e + DE_CLUS_HI, (uint16_t)(first_clus >> 16));
    fat_wr16(e + DE_CLUS_LO, (uint16_t)(first_clus & 0xFFFF));
    fat_wr32(e + DE_SIZE, size);
    fat_wr16(e + DE_WRT_TIME, time);
    fat_wr16(e + DE_WRT_DATE, date);
    fat_wr16(e + DE_ACC_DATE, date);
    e[DE_ATTR] = (uint8_t)(e[DE_ATTR] | FAT_ATTR_ARCHIVE);

    return fat_sector_write(v, s, sec);
}
