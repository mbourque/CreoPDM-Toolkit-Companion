/* textutil.c - string builder, JSON writer, UTF-8 / UTF-16 conversion, ASCII helpers, query parsing.
   No Toolkit headers; see textutil.h for the encoding rules. */
#include "textutil.h"

/* ---------- Sb ---------- */
static void sb_reserve(Sb *s, size_t extra)
{
    if (s->len + extra + 1 > s->cap) {
        size_t n = s->cap ? s->cap * 2 : 4096;
        while (n < s->len + extra + 1) n *= 2;
        s->p = (char *)realloc(s->p, n);
        s->cap = n;
    }
}
void sb_putn(Sb *s, const char *t, size_t n)
{
    sb_reserve(s, n);
    memcpy(s->p + s->len, t, n);
    s->len += n;
    s->p[s->len] = 0;
}
void sb_puts(Sb *s, const char *t) { sb_putn(s, t, strlen(t)); }
void sb_free(Sb *s) { free(s->p); s->p = NULL; s->len = s->cap = 0; }

/* ---------- UTF-8 validation ---------- */
/* length of the well-formed UTF-8 sequence starting at s (1..4), or 0 if it is not well-formed */
static int utf8_seq_len(const unsigned char *s)
{
    unsigned char c = s[0];
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) return (s[1] & 0xC0) == 0x80 ? 2 : 0;
    if (c >= 0xE0 && c <= 0xEF) {
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return 0;
        if (c == 0xE0 && s[1] < 0xA0) return 0;            /* overlong */
        if (c == 0xED && s[1] > 0x9F) return 0;            /* UTF-16 surrogates */
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) return 0;
        if (c == 0xF0 && s[1] < 0x90) return 0;            /* overlong */
        if (c == 0xF4 && s[1] > 0x8F) return 0;            /* beyond U+10FFFF */
        return 4;
    }
    return 0;
}
int utf8_valid(const char *str)
{
    const unsigned char *s = (const unsigned char *)str;
    while (*s) {
        int n = utf8_seq_len(s);
        if (!n) return 0;
        s += n;
    }
    return 1;
}

/* ---------- JSON ---------- */
static void put_json_str(Sb *s, const char *u)
{
    const unsigned char *p = (const unsigned char *)u;
    sb_puts(s, "\"");
    while (*p) {
        unsigned char c = *p;
        if (c == '"') { sb_puts(s, "\\\""); p++; }
        else if (c == '\\') { sb_puts(s, "\\\\"); p++; }
        else if (c == '\n') { sb_puts(s, "\\n"); p++; }
        else if (c == '\r') { sb_puts(s, "\\r"); p++; }
        else if (c == '\t') { sb_puts(s, "\\t"); p++; }
        else if (c < 0x20 || c == 0x7F) { char b[8]; sprintf(b, "\\u%04x", c); sb_puts(s, b); p++; }
        else if (c < 0x80) { sb_putn(s, (const char *)p, 1); p++; }
        else {
            int n = utf8_seq_len(p);
            if (n) { sb_putn(s, (const char *)p, (size_t)n); p += n; }    /* valid multi-byte character: raw UTF-8 */
            else   { sb_puts(s, "\\ufffd"); p++; }                         /* invalid byte: never let it reach the client */
        }
    }
    sb_puts(s, "\"");
}

void jw_init(Jw *w, Sb *sb) { memset(w, 0, sizeof *w); w->sb = sb; }

/* comma / newline / key prefix for the next value */
static void jw_pre(Jw *w, const char *key)
{
    if (w->need_comma[w->depth]) sb_puts(w->sb, ",");
    w->need_comma[w->depth] = 1;
    if (w->is_arr[w->depth] && w->depth <= 2) sb_puts(w->sb, "\n");
    if (key) { put_json_str(w->sb, key); sb_puts(w->sb, ": "); }
}
static void jw_open(Jw *w, const char *key, const char *br, int arr)
{
    jw_pre(w, key);
    sb_puts(w->sb, br);
    w->depth++;
    w->need_comma[w->depth] = 0;
    w->is_arr[w->depth] = (char)arr;
}
void jw_obj(Jw *w, const char *key) { jw_open(w, key, "{", 0); }
void jw_arr(Jw *w, const char *key) { jw_open(w, key, "[", 1); }
void jw_end_obj(Jw *w) { w->depth--; sb_puts(w->sb, "}"); }
void jw_end_arr(Jw *w) { w->depth--; sb_puts(w->sb, "]"); }
void jw_str(Jw *w, const char *key, const char *v)
{
    jw_pre(w, key);
    if (v) put_json_str(w->sb, v); else sb_puts(w->sb, "null");
}
void jw_wstr(Jw *w, const char *key, const wchar_t *v)
{
    char u[1024];
    if (!v) { jw_null(w, key); return; }
    w2u(v, u, sizeof u);
    jw_str(w, key, u);
}
void jw_int(Jw *w, const char *key, long long v)
{
    char b[32];
    jw_pre(w, key);
    sprintf(b, "%lld", v);
    sb_puts(w->sb, b);
}
void jw_dbl(Jw *w, const char *key, double v)
{
    char b[40];
    jw_pre(w, key);
    if (v != v || v > 1e300 || v < -1e300) { sb_puts(w->sb, "null"); return; }
    sprintf(b, "%.12g", v);
    sb_puts(w->sb, b);
}
void jw_bool(Jw *w, const char *key, int v) { jw_pre(w, key); sb_puts(w->sb, v ? "true" : "false"); }
void jw_null(Jw *w, const char *key) { jw_pre(w, key); sb_puts(w->sb, "null"); }
void jw_raw(Jw *w, const char *key, const char *raw) { jw_pre(w, key); sb_puts(w->sb, raw); }

/* ---------- encoding ---------- */
void w2u(const wchar_t *w, char *out, int outsz)
{
    int n;
    if (outsz <= 0) return;
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, outsz, NULL, NULL);
    if (n <= 0) {
        /* too long for the buffer (or unconvertible): convert in a temporary buffer and cut on a character boundary,
           so a long name is truncated instead of becoming empty */
        int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
        out[0] = 0;
        if (need > 0) {
            char *tmp = (char *)malloc((size_t)need);
            if (tmp) {
                if (WideCharToMultiByte(CP_UTF8, 0, w, -1, tmp, need, NULL, NULL) > 0) {
                    int cut = outsz - 1;
                    while (cut > 0 && ((unsigned char)tmp[cut] & 0xC0) == 0x80) cut--;   /* do not split a character */
                    memcpy(out, tmp, (size_t)cut);
                    out[cut] = 0;
                }
                free(tmp);
            }
        }
    }
    out[outsz - 1] = 0;
}

int u2w(const char *u, wchar_t *w, int wsz)
{
    if (wsz <= 0) return 0;
    w[0] = 0;
    if (!u) return 1;
    if (!utf8_valid(u)) return 0;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, u, -1, w, wsz) > 0;
}

int getenv_utf8(const char *name, char *out, int outsz)
{
    wchar_t wname[128], wval[2048];
    DWORD n;
    out[0] = 0;
    if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 128) <= 0) return 0;
    n = GetEnvironmentVariableW(wname, wval, 2048);
    if (n == 0 || n >= 2048) return 0;
    if (WideCharToMultiByte(CP_UTF8, 0, wval, -1, out, outsz, NULL, NULL) <= 0) { out[0] = 0; return 0; }
    return 1;
}

/* ---------- ASCII-only helpers ---------- */
void lower_ascii(char *s) { for (; *s; ++s) if (*s >= 'A' && *s <= 'Z') *s = (char)(*s + 32); }
void upper_ascii(char *s) { for (; *s; ++s) if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 32); }
int  ascii_alnum(int c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
int  ascii_xdigit(int c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'); }

/* ---------- query strings ---------- */
int q_get(const Query *q, const char *key, char *buf, int bufsz)
{
    const char *p = q ? q->query : NULL;
    size_t kl = strlen(key);
    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        if (seg >= kl && strncmp(p, key, kl) == 0 && (seg == kl || p[kl] == '=')) {
            size_t vl = seg > kl + 1 ? seg - kl - 1 : 0;
            if (vl >= (size_t)bufsz) vl = bufsz - 1;
            if (seg > kl + 1) memcpy(buf, p + kl + 1, vl);
            buf[vl] = 0;
            return 1;
        }
        p = amp ? amp + 1 : NULL;
    }
    return 0;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return c - 'a' + 10;
}

int q_get_dec(const Query *q, const char *key, char *buf, int bufsz)
{
    char *raw = (char *)malloc((size_t)bufsz * 3 + 8);
    int found, i, o = 0;
    if (!raw) return 0;
    found = q_get(q, key, raw, bufsz * 3);
    if (found) {
        for (i = 0; raw[i] && o < bufsz - 1; i++) {
            if (raw[i] == '%' && ascii_xdigit((unsigned char)raw[i + 1]) && ascii_xdigit((unsigned char)raw[i + 2])) {
                buf[o++] = (char)(hexval((unsigned char)raw[i + 1]) * 16 + hexval((unsigned char)raw[i + 2]));
                i += 2;
            } else if (raw[i] == '+') {
                buf[o++] = ' ';
            } else {
                buf[o++] = raw[i];
            }
        }
        buf[o] = 0;
    }
    free(raw);
    return found;
}

void url_encode(const char *in, char *out, size_t outsz)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < outsz; ++in) {
        unsigned char c = (unsigned char)*in;
        if (ascii_alnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[o++] = (char)c;
        else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
    }
    out[o] = 0;
}
