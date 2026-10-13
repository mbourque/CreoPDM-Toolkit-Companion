/* common.c - string builder, JSON writer, logging, model context */
#include "common.h"

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

/* ---------- JSON ---------- */
static void put_json_str(Sb *s, const char *u)
{
    sb_puts(s, "\"");
    for (; *u; ++u) {
        unsigned char c = (unsigned char)*u;
        if (c == '"') sb_puts(s, "\\\"");
        else if (c == '\\') sb_puts(s, "\\\\");
        else if (c == '\n') sb_puts(s, "\\n");
        else if (c == '\r') sb_puts(s, "\\r");
        else if (c == '\t') sb_puts(s, "\\t");
        else if (c < 0x20) { char b[8]; sprintf(b, "\\u%04x", c); sb_puts(s, b); }
        else sb_putn(s, (const char *)&c, 1);
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

/* ---------- misc ---------- */
void w2u(const wchar_t *w, char *out, int outsz)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, outsz, NULL, NULL);
    if (n <= 0) out[0] = 0;
    out[outsz - 1] = 0;
}
void lower_ascii(char *s) { for (; *s; ++s) *s = (char)tolower((unsigned char)*s); }

void logf_(const char *fmt, ...)
{
    char tmp[MAX_PATH], path[MAX_PATH + 32], line[512];
    SYSTEMTIME st;
    FILE *f;
    va_list ap;
    DWORD t = GetTempPathA(sizeof tmp, tmp);
    if (t == 0 || t >= sizeof tmp) return;
    _snprintf(path, sizeof path, "%screopdm_toolkit.log", tmp);
    path[sizeof path - 1] = 0;
    va_start(ap, fmt);
    _vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    line[sizeof line - 1] = 0;
    f = fopen(path, "a");
    if (!f) return;
    GetLocalTime(&st);
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d %s\n", st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, line);
    fclose(f);
}

void emit_error(Sb *out, const char *msg)
{
    Jw w;
    out->len = 0;
    if (out->p) out->p[0] = 0;
    jw_init(&w, out);
    jw_obj(&w, NULL);
    jw_bool(&w, "ok", 0);
    jw_str(&w, "error", msg);
    jw_end_obj(&w);
    sb_puts(out, "\n");
}
void emit_errorf(Sb *out, const char *fmt, ...)
{
    char m[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(m, sizeof m, fmt, ap);
    va_end(ap);
    m[sizeof m - 1] = 0;
    emit_error(out, m);
}

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

/* ---------- model context ---------- */
const char *mdl_type_str(ProMdlType t)
{
    switch (t) {
    case PRO_MDL_PART:     return "PART";
    case PRO_MDL_ASSEMBLY: return "ASSEMBLY";
    case PRO_MDL_DRAWING:  return "DRAWING";
    case PRO_MDL_MFG:      return "MFG";
    case PRO_MDL_LAYOUT:   return "LAYOUT";
    case PRO_MDL_DWGFORM:  return "DWGFORM";
    case PRO_MDL_REPORT:   return "REPORT";
    case PRO_MDL_MARKUP:   return "MARKUP";
    case PRO_MDL_DIAGRAM:  return "DIAGRAM";
    default:               return "OTHER";
    }
}
static const char *mdl_ext(ProMdlType t)
{
    switch (t) {
    case PRO_MDL_PART:     return ".prt";
    case PRO_MDL_ASSEMBLY: return ".asm";
    case PRO_MDL_DRAWING:  return ".drw";
    case PRO_MDL_MFG:      return ".mfg";
    case PRO_MDL_LAYOUT:   return ".lay";
    case PRO_MDL_DWGFORM:  return ".frm";
    case PRO_MDL_DIAGRAM:  return ".dgm";
    default:               return "";
    }
}

int q_get_dec(const Query *q, const char *key, char *buf, int bufsz)
{
    char *raw = (char *)malloc(bufsz * 3 + 8);
    int found, i, o = 0;
    if (!raw) return 0;
    found = q_get(q, key, raw, bufsz * 3);
    if (found) {
        for (i = 0; raw[i] && o < bufsz - 1; i++) {
            if (raw[i] == '%' && isxdigit((unsigned char)raw[i + 1]) && isxdigit((unsigned char)raw[i + 2])) {
                char h[3] = { raw[i + 1], raw[i + 2], 0 };
                buf[o++] = (char)strtol(h, NULL, 16);
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

int ctx_load(Ctx *c)
{
    ProMdl cur = NULL;
    memset(c, 0, sizeof *c);
    c->typeStr = "NONE";
    if (ProMdlCurrentGet(&cur) != PRO_TK_NO_ERROR || !cur) return 0;
    return ctx_for_model(c, cur);
}

int ctx_for_model(Ctx *c, ProMdl mdl)
{
    ProMdlName nm;
    char u[200];
    memset(c, 0, sizeof *c);
    c->typeStr = "NONE";
    c->mdl = mdl;
    if (!c->mdl) return 0;
    if (ProMdlTypeGet(c->mdl, &c->type) != PRO_TK_NO_ERROR) return 0;
    u[0] = 0;
    if (ProMdlMdlnameGet(c->mdl, nm) == PRO_TK_NO_ERROR) w2u(nm, u, sizeof u);
    if (u[0]) {
        _snprintf(c->fileName, sizeof c->fileName, "%s%s", u, mdl_ext(c->type));
        c->fileName[sizeof c->fileName - 1] = 0;
        lower_ascii(c->fileName);
    }   /* else fileName stays empty: caller may fall back to the requested tip */
    c->typeStr = mdl_type_str(c->type);
    c->has = 1;
    return 1;
}

int is_noname_u(const char *u)
{
    return u && _stricmp(u, "no_name") == 0;
}

/* When the handle gave no usable name, use the tip the caller asked for (e.g. "shaft.prt" or "shaft.prt.2"). */
void ctx_fallback_name(Ctx *c, const char *requested)
{
    if (c->has && !c->fileName[0] && requested && requested[0]) {
        strncpy(c->fileName, requested, sizeof c->fileName - 1);
        c->fileName[sizeof c->fileName - 1] = 0;
        lower_ascii(c->fileName);
    }
}
/* True if the file header says this is a cabling harness part ("#UGC:2 PART/HARNESS_PART ...").
   Only the first line (<= 200 bytes) is read; any read problem means "not detected". */
int file_is_harness_w(const wchar_t *wpath)
{
    FILE *f = _wfopen(wpath, L"rb");
    char hdr[201];
    size_t n, i;
    if (!f) return 0;
    n = fread(hdr, 1, 200, f);
    fclose(f);
    hdr[n] = 0;
    for (i = 0; i < n; i++) if (hdr[i] == 10 || hdr[i] == 13) { hdr[i] = 0; break; }
    return strncmp(hdr, "#UGC:", 5) == 0 && strstr(hdr, "HARNESS") != NULL;
}
