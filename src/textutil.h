/* textutil.h - text helpers with NO Toolkit dependency (so they can be unit-tested on their own).
 *
 * Encoding rules used everywhere in the service:
 *   - Creo strings are UTF-16 (wchar_t). They become UTF-8 only through w2u() (WideCharToMultiByte, CP_UTF8).
 *   - Everything stored in char buffers, JSON bodies, log lines and URLs is UTF-8.
 *   - Case folding and character tests on those buffers are ASCII-ONLY (lower_ascii, upper_ascii, ascii_*): the C
 *     library's tolower/toupper/isalnum/isxdigit depend on the process locale, and in a Windows-1252 locale they
 *     rewrite UTF-8 lead bytes (e.g. 0xC3 -> 0xE3), which corrupts non-ASCII names.
 *   - The JSON writer never emits invalid UTF-8: any invalid byte is written as the escape �.
 */
#ifndef CPDM_TEXTUTIL_H
#define CPDM_TEXTUTIL_H

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ---------- growable string ---------- */
typedef struct { char *p; size_t len, cap; } Sb;
void sb_puts(Sb *s, const char *t);
void sb_putn(Sb *s, const char *t, size_t n);
void sb_free(Sb *s);

/* ---------- JSON writer (auto commas, always valid UTF-8) ---------- */
typedef struct {
    Sb  *sb;
    int  depth;
    char need_comma[40];
    char is_arr[40];
} Jw;
void jw_init(Jw *w, Sb *sb);
void jw_obj(Jw *w, const char *key);      /* key NULL inside arrays / at root */
void jw_end_obj(Jw *w);
void jw_arr(Jw *w, const char *key);
void jw_end_arr(Jw *w);
void jw_str(Jw *w, const char *key, const char *v);   /* v NULL -> null; v is UTF-8 */
void jw_int(Jw *w, const char *key, long long v);
void jw_dbl(Jw *w, const char *key, double v);
void jw_bool(Jw *w, const char *key, int v);
void jw_null(Jw *w, const char *key);
void jw_raw(Jw *w, const char *key, const char *raw);
void jw_wstr(Jw *w, const char *key, const wchar_t *v);

/* ---------- encoding ---------- */
void w2u(const wchar_t *w, char *out, int outsz);    /* UTF-16 -> UTF-8; truncates on a character boundary, never empties */
int  u2w(const char *u, wchar_t *w, int wsz);        /* UTF-8 -> UTF-16; returns 0 if u is not valid UTF-8 (or too long) */
int  utf8_valid(const char *s);                      /* 1 if s is well-formed UTF-8 */
int  getenv_utf8(const char *name, char *out, int outsz);   /* wide environment read, returned as UTF-8; 0 if unset/too long */

/* ---------- ASCII-only helpers (locale independent) ---------- */
void lower_ascii(char *s);
void upper_ascii(char *s);
int  ascii_alnum(int c);
int  ascii_xdigit(int c);

/* ---------- query strings ---------- */
typedef struct {
    const char *query;      /* raw query string without '?', may be "" */
} Query;
int  q_get(const Query *q, const char *key, char *buf, int bufsz);       /* 1 if present (raw) */
int  q_get_dec(const Query *q, const char *key, char *buf, int bufsz);   /* percent-decoded (%XX, +) to bytes (UTF-8 expected) */
void url_encode(const char *in, char *out, size_t outsz);                /* UTF-8 bytes -> %XX, ASCII alnum and -_.~ kept */

#endif
