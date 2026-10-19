/* common.c - logging, error envelopes, model context (text/JSON helpers are in textutil.c) */
#include "common.h"
#include <ProUtil.h>
#include <ProArray.h>


/* ---------- misc ---------- */

void logf_(const char *fmt, ...)
{
    wchar_t tmp[MAX_PATH], path[MAX_PATH + 32];
    char line[512];
    SYSTEMTIME st;
    FILE *f;
    va_list ap;
    DWORD t = GetTempPathW(MAX_PATH, tmp);          /* wide: a temp folder under a non-ASCII user name works too */
    if (t == 0 || t >= MAX_PATH) return;
    _snwprintf(path, MAX_PATH + 32, L"%lscreopdm_toolkit.log", tmp);
    path[MAX_PATH + 31] = 0;
    va_start(ap, fmt);
    _vsnprintf(line, sizeof line, fmt, ap);        /* the message text is UTF-8 and is written as is */
    va_end(ap);
    line[sizeof line - 1] = 0;
    f = _wfopen(path, L"a");
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
    case PRO_MDL_2DSECTION: return "SECTION";
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
    case PRO_MDL_2DSECTION: return ".sec";
    default:               return "";
    }
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
        lower_utf8(c->fileName, sizeof c->fileName);
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
        lower_utf8(c->fileName, sizeof c->fileName);
    }
}
/* True if the file header says this is a cabling harness part ("#UGC:2 PART/HARNESS_PART ...").
   Only the first line (<= 200 bytes) is read; any read problem means "not detected". */
/* Release the running Creo reports (user_initialize's version string), e.g. for identity.creo_release */
char g_creo_version[64];

/* Release the FILE was saved with: the second header line reads "#Creo  TM  13  (c) 2026 by PTC Inc.  All Rights Reserved. 13.4.1.0".
   Returns 1 and fills out ("13.4.1.0") when found. */
int file_saved_release_w(const wchar_t *wpath, char *out, int outsz)
{
    FILE *f = _wfopen(wpath, L"rb");
    char buf[1025], *p, *eol, *last;
    size_t n;
    out[0] = 0;
    if (!f) return 0;
    n = fread(buf, 1, 1024, f);
    fclose(f);
    buf[n] = 0;
    p = strstr(buf, "#Creo");
    if (!p) return 0;
    eol = p;
    while (*eol && *eol != 10 && *eol != 13) eol++;
    *eol = 0;
    last = strrchr(p, ' ');
    if (!last || !last[1]) return 0;
    strncpy(out, last + 1, outsz - 1);
    out[outsz - 1] = 0;
    return out[0] != 0;
}

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

/* ---- running Creo release as a dotted string ("13.4.1.0"), the same form Creo writes in a saved model's header ---- */
char g_creo_release[32];

/* first "d.d.d.d" in s (digits and dots only, exactly four numbers), or 0 */
static int find_dotted4(const char *s, char *out, int outsz)
{
    const char *p = s;
    while (*p) {
        if (*p >= '0' && *p <= '9' && (p == s || !((p[-1] >= '0' && p[-1] <= '9') || p[-1] == '.'))) {
            const char *q = p;
            int dots = 0;
            while ((*q >= '0' && *q <= '9') || (*q == '.' && q[1] >= '0' && q[1] <= '9')) { if (*q == '.') dots++; q++; }
            if (dots == 3 && (int)(q - p) < outsz) { memcpy(out, p, (size_t)(q - p)); out[q - p] = 0; return 1; }
            p = q;
        } else p++;
    }
    return 0;
}

void creo_release_init(void)
{
    char *dc = NULL;
    if (g_creo_release[0]) return;
    if (ProEngineerDisplaydatecodeGet(&dc) == PRO_TK_NO_ERROR && dc) {
        logf_("Creo datecode string: '%s'", dc);
        if (!find_dotted4(dc, g_creo_release, sizeof g_creo_release)) g_creo_release[0] = 0;   /* not in dotted form: do not guess */
        ProStringFree(dc);
    }
}

/* -1 / 0 / 1 comparing dotted releases numerically; 2 if either is not parseable */
int release_compare(const char *a, const char *b)
{
    int x[4] = { 0 }, y[4] = { 0 }, i;
    if (sscanf(a, "%d.%d.%d.%d", &x[0], &x[1], &x[2], &x[3]) != 4) return 2;
    if (sscanf(b, "%d.%d.%d.%d", &y[0], &y[1], &y[2], &y[3]) != 4) return 2;
    for (i = 0; i < 4; i++) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}
