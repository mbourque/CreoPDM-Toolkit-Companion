/* router.c - maps paths to collectors; /health and /collect. Runs on Creo's main thread. */
#include "common.h"
#include <ProWindows.h>

#define M_P 1
#define M_A 2
#define M_M 4
#define M_D 8
#define M_ALL (M_P | M_A | M_M | M_D)

/* mask = types the domain applies to; all_mask = types it is included for in a default /collect */
typedef struct { const char *name; CollectFn fn; unsigned mask; unsigned all_mask; } Domain;

/* name uses '_' ; URL path and ?domains= accept '-' too (simp-reps / simp_reps) */
static const Domain DOMAINS[] = {
    { "identity",    collect_identity,    M_ALL, M_ALL },
    { "units",       collect_units,       M_P | M_A | M_M, M_P | M_A | M_M },
    { "parameters",  collect_parameters,  M_ALL, M_ALL },
    { "mass",        collect_mass,        M_P | M_A, M_P | M_A },
    { "materials",   collect_materials,   M_P, M_P },
    { "features",    collect_features,    M_P | M_A | M_D, M_P | M_A },   /* DRAWING: GET /features aliases /drawing; omitted from default collect */
    { "relations",   collect_relations,   M_P | M_A, M_P | M_A },
    { "structure",   collect_structure,   M_A, M_A },
    { "constraints", collect_constraints, M_A, M_A },
    { "simp_reps",   collect_simpreps,    M_A, M_A },
    { "drawing",     collect_drawing,     M_D, M_D },
    { "mfg",         collect_mfg,         M_M, M_M },
    { "bom",         collect_bom,         M_A, 0 },      /* explicit only: not part of a default /collect */
    { "harness_probe", collect_harness_probe, M_A, 0 },   /* diagnostic, explicit only: GET /harness-probe */
};
#define NDOM ((int)(sizeof DOMAINS / sizeof DOMAINS[0]))

/* ---------- per-request timings (main thread only; requests are serialized) ---------- */
typedef struct {
    int    retrieved;
    double retrieve_ms, erase_ms, total_ms;
    double dom_ms[NDOM];
    char   dom_ran[NDOM];
} Timings;
static Timings g_tm;

static double now_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
}

/* Append "timings_ms" to a complete JSON object that ends in "}\n". */
static void append_timings(Sb *out)
{
    Jw w;
    Sb tm = { 0 };
    char tail[64];
    int i;
    if (!out->p || out->len < 2) return;
    while (out->len && (out->p[out->len - 1] == '\n' || out->p[out->len - 1] == '\r')) out->p[--out->len] = 0;
    if (out->len == 0 || out->p[out->len - 1] != '}') return;
    out->p[--out->len] = 0;                       /* drop closing brace */

    jw_init(&w, &tm);
    jw_obj(&w, NULL);
    jw_bool(&w, "retrieved", g_tm.retrieved);
    jw_dbl(&w, "retrieve_ms", g_tm.retrieve_ms);
    for (i = 0; i < NDOM; i++) {
        if (g_tm.dom_ran[i]) {
            char key[48];
            sprintf(key, "%s_ms", DOMAINS[i].name);
            jw_dbl(&w, key, g_tm.dom_ms[i]);
        }
    }
    jw_dbl(&w, "erase_ms", g_tm.erase_ms);
    jw_dbl(&w, "total_ms", g_tm.total_ms);
    jw_end_obj(&w);

    sb_puts(out, ", \"timings_ms\": ");
    sb_puts(out, tm.p);
    sprintf(tail, "}\n");
    sb_puts(out, tail);
    sb_free(&tm);
}

static void log_timings(const char *path, const char *fileName)
{
    char b[600];
    int i, n;
    n = sprintf(b, "%s %s retrieved=%d retrieve=%.0fms", path, fileName ? fileName : "(active)",
                g_tm.retrieved, g_tm.retrieve_ms);
    for (i = 0; i < NDOM && n < 450; i++)
        if (g_tm.dom_ran[i]) n += sprintf(b + n, " %s=%.0fms", DOMAINS[i].name, g_tm.dom_ms[i]);
    sprintf(b + n, " erase=%.0fms total=%.0fms", g_tm.erase_ms, g_tm.total_ms);
    logf_("%s", b);
}

static unsigned type_bit(ProMdlType t)
{
    switch (t) {
    case PRO_MDL_PART:     return M_P;
    case PRO_MDL_ASSEMBLY: return M_A;
    case PRO_MDL_MFG:      return M_M;
    case PRO_MDL_DRAWING:  return M_D;
    default:               return 0;
    }
}

static const Domain *find_domain(const char *s)
{
    char n[48];
    int i;
    for (i = 0; s[i] && i < (int)sizeof n - 1; i++) n[i] = (s[i] == '-') ? '_' : s[i];
    n[i] = 0;
    for (i = 0; i < NDOM; i++) if (strcmp(DOMAINS[i].name, n) == 0) return &DOMAINS[i];
    return NULL;
}

static void trim_nl(Sb *s)
{
    while (s->len && (s->p[s->len - 1] == '\n' || s->p[s->len - 1] == '\r')) s->p[--s->len] = 0;
}

static void do_health(Sb *out, const Ctx *c)
{
    Jw w;
    jw_init(&w, out);
    jw_obj(&w, NULL);
    jw_bool(&w, "ok", 1);
    jw_str(&w, "service", CPDM_SERVICE);
    jw_str(&w, "version", CPDM_VERSION);
    if (c->has) {
        jw_obj(&w, "model");
        jw_str(&w, "fileName", c->fileName);
        jw_str(&w, "type", c->typeStr);
        jw_end_obj(&w);
    } else {
        jw_null(&w, "model");
    }
    jw_end_obj(&w);
    sb_puts(out, "\n");
}

static void do_collect(Sb *out, const Ctx *c, const Query *q)
{
    char dom[256], tok[48];
    Jw w, bw;
    Sb body = { 0 };
    int explicit_list = q_get(q, "domains", dom, sizeof dom) && dom[0] && strcmp(dom, "all") != 0;
    unsigned tb = type_bit(c->type);

    jw_init(&bw, &body);
    jw_obj(&bw, NULL);
    if (!explicit_list) {
        int i;
        for (i = 0; i < NDOM; i++) {
            Sb r = { 0 };
            if (!(DOMAINS[i].all_mask & tb)) continue;   /* skip domains that do not apply */
            { double t0 = now_ms(); DOMAINS[i].fn(&r, c, q); g_tm.dom_ms[i] = now_ms() - t0; g_tm.dom_ran[i] = 1; }
            trim_nl(&r);
            jw_raw(&bw, DOMAINS[i].name, r.p ? r.p : "null");
            sb_free(&r);
        }
    } else {
        const char *p = dom;
        while (*p) {
            const char *comma = strchr(p, ',');
            size_t n = comma ? (size_t)(comma - p) : strlen(p);
            if (n >= sizeof tok) n = sizeof tok - 1;
            memcpy(tok, p, n);
            tok[n] = 0;
            if (n) {
                Sb r = { 0 };
                const Domain *d = find_domain(tok);
                if (!d) {
                    emit_errorf(&r, "unknown domain '%s'", tok);
                    trim_nl(&r);
                    jw_raw(&bw, tok, r.p);
                } else {
                    { double t0 = now_ms(); int di = (int)(d - DOMAINS); d->fn(&r, c, q); g_tm.dom_ms[di] = now_ms() - t0; g_tm.dom_ran[di] = 1; }   /* collector reports its own not-applicable error */
                    trim_nl(&r);
                    jw_raw(&bw, d->name, r.p ? r.p : "null");
                }
                sb_free(&r);
            }
            p = comma ? comma + 1 : p + strlen(p);
        }
    }
    jw_end_obj(&bw);

    jw_init(&w, out);
    jw_obj(&w, NULL);
    jw_bool(&w, "ok", 1);
    jw_str(&w, "fileName", c->fileName);
    jw_str(&w, "type", c->typeStr);
    jw_raw(&w, "domains", body.p);
    jw_end_obj(&w);
    sb_puts(out, "\n");
    sb_free(&body);
}

/* "name.prt", "name.prt.12", "NAME.ASM.3" -> base name + file type. Returns 0 if not a known type. */
static int parse_file_name(const char *fn, ProMdlName base, ProMdlfileType *ft)
{
    char tmp[300], *dot, *end;
    size_t n = strlen(fn);
    if (n == 0 || n >= sizeof tmp) return 0;
    strcpy(tmp, fn);
    lower_ascii(tmp);
    /* strip a trailing .N version */
    end = tmp + strlen(tmp);
    dot = strrchr(tmp, '.');
    if (dot && dot[1] && strspn(dot + 1, "0123456789") == strlen(dot + 1)) { *dot = 0; end = dot; }
    dot = strrchr(tmp, '.');
    if (!dot) return 0;
    if (strcmp(dot, ".prt") == 0) *ft = PRO_MDLFILE_PART;
    else if (strcmp(dot, ".asm") == 0) *ft = PRO_MDLFILE_ASSEMBLY;
    else if (strcmp(dot, ".drw") == 0) *ft = PRO_MDLFILE_DRAWING;
    else if (strcmp(dot, ".mfg") == 0) *ft = PRO_MDLFILE_MFG;
    else return 0;
    *dot = 0;
    (void)end;
    if (!u2w(tmp, base, PRO_NAME_SIZE)) return 0;
    return 1;
}

/* Set by resolve_target when the requested file is a cabling harness (main thread only). */
static int  g_is_harness;
static char g_harness_full[1700];


/* Find the model in session, else retrieve it SILENTLY from path (ProMdlFiletypeLoad:
   no display, no window, not made current). */
static int resolve_target(const char *fn, const char *path, ProMdl *out_mdl, int *retrieved,
                          char *err, int errsz)
{
    g_is_harness = 0; g_harness_full[0] = 0;
    ProMdlName base;
    ProMdlfileType ft = PRO_MDLFILE_UNUSED;
    ProMdl m = NULL;
    ProPath wp;
    ProError rc;
    char full[1700];
    size_t pl;

    *retrieved = 0;
    if (!parse_file_name(fn, base, &ft)) {
        _snprintf(err, errsz, "fileName '%s' must end in .prt/.asm/.drw/.mfg (optionally .N), be valid UTF-8 and fit the name length", fn);
        err[errsz - 1] = 0;
        return 0;
    }
    if (ProMdlnameInit(base, ft, &m) == PRO_TK_NO_ERROR && m) {   /* already in session */
        *out_mdl = m;
        return 1;
    }
    if (!path[0]) {
        _snprintf(err, errsz, "%s is not in session; pass path= (absolute path to the file) so it can be "
                  "retrieved in the background", fn);
        err[errsz - 1] = 0;
        return 0;
    }
    /* path may be the file itself or its folder */
    strncpy(full, path, sizeof full - 1);
    full[sizeof full - 1] = 0;
    pl = strlen(full);
    if (pl && (full[pl - 1] == '\\' || full[pl - 1] == '/')) {
        strncat(full, fn, sizeof full - pl - 1);
    }
    if (!u2w(full, wp, PRO_PATH_SIZE)) {
        _snprintf(err, errsz, "path is not valid UTF-8 or is too long (percent-encode non-ASCII characters as UTF-8)");
        err[errsz - 1] = 0;
        return 0;
    }
    /* Cabling harness parts cannot be opened in isolation (Creo: "open the cabling assembly which contains
       this harness"). Toolkit has no API to inspect a model without loading it, so read the file header line:
       a harness is saved as "#UGC:2 PART/HARNESS_PART ...". Rejecting here avoids the load (and the Creo UI message). */
    if (file_is_harness_w(wp)) {
        g_is_harness = 1; strncpy(g_harness_full, full, sizeof g_harness_full - 1);
        _snprintf(err, errsz, "harness_not_openable_in_isolation: open the cabling assembly that contains %s", fn);
        err[errsz - 1] = 0;
        return 0;
    }
    rc = ProMdlFiletypeLoad(wp, ft, PRO_B_FALSE, &m);
    if (rc != PRO_TK_NO_ERROR || !m) {
        DWORD attr = GetFileAttributesW(wp);
        if (attr == INVALID_FILE_ATTRIBUTES)
            _snprintf(err, errsz, "file_not_found: %s (ProMdlFiletypeLoad=%d)", full, (int)rc);
        else if (file_is_harness_w(wp)) {   /* same check after a failed load, in case it could not run before */
            g_is_harness = 1; strncpy(g_harness_full, full, sizeof g_harness_full - 1);
            _snprintf(err, errsz, "harness_not_openable_in_isolation: open the cabling assembly that contains %s", fn);
        }
        else
            _snprintf(err, errsz, "could not retrieve %s from path (ProMdlFiletypeLoad=%d)", fn, (int)rc);
        err[errsz - 1] = 0;
        return 0;
    }
    *retrieved = 1;
    *out_mdl = m;
    return 1;
}

/* ---------- cabling harness stub (file-header detection, nothing loaded) ---------- */
static void harness_skip(Sb *r, const char *fn)
{
    emit_errorf(r, "harness_not_openable_in_isolation: open the cabling assembly that contains %s", fn);
}

/* Minimal identity from the file header only: no model handle exists. */
static void harness_identity(Sb *r, const char *fn, const char *full)
{
    Jw w;
    char tip[300];
    strncpy(tip, fn, sizeof tip - 1);
    tip[sizeof tip - 1] = 0;
    lower_ascii(tip);
    jw_init(&w, r);
    jw_obj(&w, NULL);
    jw_bool(&w, "ok", 1);
    jw_str(&w, "fileName", tip);
    jw_obj(&w, "identity");
    jw_str(&w, "file_name", tip);
    jw_str(&w, "model_type", "PART");
    jw_str(&w, "model_role", "HARNESS");
    jw_str(&w, "subtype", "HARNESS");
    jw_str(&w, "origin", full);
    jw_str(&w, "identity_source", "file_header");   /* not read from a loaded model */
    jw_end_obj(&w);
    jw_end_obj(&w);
    sb_puts(r, "\n");
}

/* Response for a harness tip: identity succeeds; every domain that needs a loaded model is skipped. */
static int route_harness(const char *path, const Query *q, const char *fn, const char *full, Sb *out)
{
    char dom[256], tok[48];
    Jw w, bw;
    Sb body = { 0 };
    int i, first_all;

    if (strcmp(path, "/identity") == 0) { harness_identity(out, fn, full); return 200; }
    if (strcmp(path, "/collect") != 0) { harness_skip(out, fn); return 200; }

    first_all = !(q_get(q, "domains", dom, sizeof dom) && dom[0] && strcmp(dom, "all") != 0);
    jw_init(&bw, &body);
    jw_obj(&bw, NULL);
    if (first_all) {
        for (i = 0; i < NDOM; i++) {
            Sb r = { 0 };
            if (!(DOMAINS[i].all_mask & M_P)) continue;          /* what a PART collect would include */
            if (strcmp(DOMAINS[i].name, "identity") == 0) harness_identity(&r, fn, full);
            else harness_skip(&r, fn);
            trim_nl(&r);
            jw_raw(&bw, DOMAINS[i].name, r.p);
            sb_free(&r);
        }
    } else {
        const char *p = dom;
        while (*p) {
            const char *comma = strchr(p, ',');
            size_t n = comma ? (size_t)(comma - p) : strlen(p);
            if (n >= sizeof tok) n = sizeof tok - 1;
            memcpy(tok, p, n);
            tok[n] = 0;
            if (n) {
                Sb r = { 0 };
                const Domain *d = find_domain(tok);
                if (!d) emit_errorf(&r, "unknown domain '%s'", tok);
                else if (strcmp(d->name, "identity") == 0) harness_identity(&r, fn, full);
                else harness_skip(&r, fn);
                trim_nl(&r);
                jw_raw(&bw, d ? d->name : tok, r.p);
                sb_free(&r);
            }
            p = comma ? comma + 1 : p + strlen(p);
        }
    }
    jw_end_obj(&bw);

    {
        char tip[300];
        strncpy(tip, fn, sizeof tip - 1);
        tip[sizeof tip - 1] = 0;
        lower_ascii(tip);
        jw_init(&w, out);
        jw_obj(&w, NULL);
        jw_bool(&w, "ok", 1);
        jw_str(&w, "fileName", tip);
        jw_str(&w, "type", "PART");
        jw_bool(&w, "harness", 1);
        jw_raw(&w, "domains", body.p);
        jw_end_obj(&w);
        sb_puts(out, "\n");
    }
    sb_free(&body);
    return 200;
}

/* ---------- /open: open a model from disk in a Creo window (used by the CreoPDM explorer via the agent) ----------
   GET /open?fileName=<name.ext[.N]>&path=<absolute path to the file>
   Loads the model silently if it is not in session (same resolve as /collect: harness / not-found errors apply), then
   shows it the way File > Open does: a new window, made active. If the model is already shown in a window, that window
   is activated instead (no duplicate tabs). The model is NOT erased afterwards. */
static int do_open(const Query *q, Sb *out)
{
    char fn[300], pth[1600], emsg[400];
    ProMdl target = NULL;
    int retrieved = 0, win = 0, already = 0;
    ProMdlName nm;
    Ctx c;
    ProError rc;
    Jw w;

    fn[0] = pth[0] = 0;
    q_get_dec(q, "fileName", fn, sizeof fn);
    q_get_dec(q, "path", pth, sizeof pth);
    if (!fn[0] && pth[0]) {                         /* derive the tip from the path */
        const char *s = strrchr(pth, 92);              /* backslash */
        const char *s2 = strrchr(pth, '/');
        if (s2 > s) s = s2;
        strncpy(fn, s ? s + 1 : pth, sizeof fn - 1);
        fn[sizeof fn - 1] = 0;
    }
    if (!fn[0]) { emit_error(out, "pass fileName= and/or path= to open a model"); return 200; }

    if (!resolve_target(fn, pth, &target, &retrieved, emsg, sizeof emsg)) { emit_error(out, emsg); return 200; }
    ctx_for_model(&c, target);
    if (!c.has) { emit_errorf(out, "model handle for %s is not usable", fn); return 200; }

    if (ProMdlWindowGet(target, &win) == PRO_TK_NO_ERROR && win > 0) {
        already = 1;                                 /* already displayed: just bring it forward */
        rc = ProWindowActivate(win);
    } else if (ProMdlMdlnameGet(target, nm) != PRO_TK_NO_ERROR) {
        rc = PRO_TK_GENERAL_ERROR;
    } else {
        rc = ProObjectwindowMdlnameCreate(nm, (ProType)c.type, &win);   /* new window, like File > Open */
        if (rc == PRO_TK_NO_ERROR) rc = ProWindowActivate(win);
        if (rc == PRO_TK_NO_ERROR) rc = ProMdlDisplay(target);
    }
    logf_("/open %s retrieved=%d already=%d window=%d -> %d", c.fileName, retrieved, already, win, (int)rc);
    if (rc != PRO_TK_NO_ERROR) {
        emit_errorf(out, "could not display %s in a window (Toolkit error %d)", c.fileName, (int)rc);
        return 200;
    }

    jw_init(&w, out);
    jw_obj(&w, NULL);
    jw_bool(&w, "ok", 1);
    jw_str(&w, "fileName", c.fileName);
    jw_str(&w, "type", c.typeStr);
    jw_int(&w, "window", win);
    jw_bool(&w, "alreadyOpen", already);
    jw_bool(&w, "loadedFromDisk", retrieved);
    jw_end_obj(&w);
    sb_puts(out, "\n");
    return 200;
}

/* ---------- /url: load an address in Creo's main embedded browser (not the navigator pane) ----------
   GET /url?url=<absolute http(s) URL, percent-encoded>[&percent=10..95]
   Uses ProWindowURLShow on the current window. Safety: only http/https; if CREOPDM_URL is set the address must be on
   the same scheme://host:port. The browser is widened to `percent` of the window (default env CREOPDM_BROWSER_PERCENT or 50)
   only if it is currently narrower; it is never shrunk. */
static void origin_of(const char *url, char *out, size_t outsz)
{
    const char *p = strstr(url, "://");
    size_t n = 0;
    out[0] = 0;
    if (!p) return;
    p += 3;
    n = (size_t)(p - url);
    while (p[0] && p[0] != '/' && p[0] != '?' && p[0] != '#') p++;
    n = (size_t)(p - url);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, url, n);
    out[n] = 0;
    lower_ascii(out);
}

static int do_url(const Query *q, Sb *out)
{
    char url[1600], pct[16], cfg[1100], o1[400], o2[400];
    wchar_t wurl[1600];
    double target = 50.0, cur = 0.0;
    int win = 0, widened = 0;
    ProError rc;
    Jw w;

    url[0] = 0;
    q_get_dec(q, "url", url, sizeof url);
    if (!url[0]) { emit_error(out, "pass url=<absolute http(s) address, percent-encoded>"); return 200; }
    if (_strnicmp(url, "http://", 7) != 0 && _strnicmp(url, "https://", 8) != 0) {
        emit_error(out, "url must start with http:// or https://");
        return 200;
    }
    if (getenv_utf8("CREOPDM_URL", cfg, sizeof cfg) && cfg[0]) {     /* same origin as CreoPDM */
        origin_of(url, o1, sizeof o1);
        origin_of(cfg, o2, sizeof o2);
        if (strcmp(o1, o2) != 0) {
            emit_errorf(out, "url must be on the configured CreoPDM address (%s)", o2);
            return 200;
        }
    }
    if (GetEnvironmentVariableA("CREOPDM_BROWSER_PERCENT", pct, sizeof pct) > 0) {
        double p = atof(pct);
        if (p >= 10.0 && p <= 95.0) target = p;
    }
    if (q_get(q, "percent", pct, sizeof pct)) {
        double p = atof(pct);
        if (p >= 10.0 && p <= 95.0) target = p;
    }

    if (!u2w(url, wurl, 1600)) { emit_error(out, "url is not valid UTF-8 (percent-encode it as UTF-8)"); return 200; }
    rc = ProWindowCurrentGet(&win);
    if (rc == PRO_TK_NO_ERROR) rc = ProWindowURLShow(win, wurl);
    logf_("/url %s -> %d (window %d)", url, (int)rc, win);
    if (rc != PRO_TK_NO_ERROR) {
        emit_errorf(out, "could not load the address in Creo's browser (Toolkit error %d)", (int)rc);
        return 200;
    }
    if (ProWindowBrowserSizeGet(win, &cur) != PRO_TK_NO_ERROR || cur < target)       /* widen, never shrink */
        widened = (ProWindowBrowserSizeSet(win, target) == PRO_TK_NO_ERROR);

    jw_init(&w, out);
    jw_obj(&w, NULL);
    jw_bool(&w, "ok", 1);
    jw_str(&w, "url", url);
    jw_int(&w, "window", win);
    jw_bool(&w, "widened", widened);
    jw_end_obj(&w);
    sb_puts(out, "\n");
    return 200;
}

int route_request(const char *method, const char *path, const char *query, Sb *out)
{
    Ctx c;
    Query q;
    const Domain *d;
    q.query = query ? query : "";

    if (_stricmp(method, "GET") != 0 && _stricmp(method, "POST") != 0) {
        emit_error(out, "method not allowed");
        return 405;
    }
    ctx_load(&c);

    if (strcmp(path, "/health") == 0) { do_health(out, &c); return 200; }

    /* /model: lightweight model info (same data as health.model) for every model type */
    if (strcmp(path, "/model") == 0) {
        Jw w;
        if (!c.has) { emit_error(out, "No active model in session"); return 200; }
        jw_init(&w, out);
        jw_obj(&w, NULL);
        jw_bool(&w, "ok", 1);
        jw_str(&w, "fileName", c.fileName);
        jw_str(&w, "type", c.typeStr);
        jw_obj(&w, "model");
        jw_str(&w, "fileName", c.fileName);
        jw_str(&w, "type", c.typeStr);
        jw_end_obj(&w);
        jw_end_obj(&w);
        sb_puts(out, "\n");
        return 200;
    }

    if (strcmp(path, "/open") == 0) return do_open(&q, out);   /* needs no active model */
    if (strcmp(path, "/url") == 0) return do_url(&q, out);     /* needs no active model */
    if (strcmp(path, "/collect") != 0 && !((path[0] == '/') && find_domain(path + 1))) {
        emit_error(out, "not found");
        return 404;
    }

    /* Optional target: ?fileName=<name.ext[.N]>&path=<absolute path to the file>.
       Without fileName the active model is used (as before). */
    {
        char fn[300], pth[1600], keep[8], emsg[400];
        ProMdl target = NULL;
        int retrieved = 0;
        double t_start = now_ms(), t1;

        memset(&g_tm, 0, sizeof g_tm);
        fn[0] = 0;
        if (q_get_dec(&q, "fileName", fn, sizeof fn) && fn[0]) {
            pth[0] = 0;
            q_get_dec(&q, "path", pth, sizeof pth);
            t1 = now_ms();
            if (!resolve_target(fn, pth, &target, &retrieved, emsg, sizeof emsg)) {
                if (g_is_harness) route_harness(path, &q, fn, g_harness_full, out);   /* identity ok, rest skipped */
                else emit_error(out, emsg);
                g_tm.retrieve_ms = now_ms() - t1;
                g_tm.total_ms = now_ms() - t_start;
                append_timings(out);
                log_timings(path, fn);
                return 200;
            }
            g_tm.retrieve_ms = now_ms() - t1;    /* ~0 when the model was already in session */
            g_tm.retrieved = retrieved;
            ctx_for_model(&c, target);
            if (!c.has) {
                emit_errorf(out, "model handle for %s is not usable (type could not be read)", fn);
                g_tm.total_ms = now_ms() - t_start;
                append_timings(out);
                log_timings(path, fn);
                return 200;
            }
            ctx_fallback_name(&c, fn);       /* identity.file_name must never be empty: use the requested tip */
        }
        if (!c.has) { emit_error(out, "No active model in session"); return 200; }
        if (!c.fileName[0]) { emit_error(out, "model has no usable name (handle returned none and no fileName was requested)"); return 200; }

        if (strcmp(path, "/collect") == 0) {
            do_collect(out, &c, &q);
        } else {
            d = find_domain(path + 1);
            { double t0 = now_ms(); int di = (int)(d - DOMAINS); d->fn(out, &c, &q); g_tm.dom_ms[di] = now_ms() - t0; g_tm.dom_ran[di] = 1; }
        }

        /* Erase only a model THIS call retrieved (so it cannot be one the user has displayed);
           ?keep=1 leaves it in session. Dependencies pulled in with it stay in session, undisplayed. */
        if (retrieved && target && !(q_get(&q, "keep", keep, sizeof keep) && keep[0] == '1')) {
            ProMdl cur = NULL;
            double te = now_ms();
            ProMdlCurrentGet(&cur);
            if (cur != target) ProMdlErase(target);   /* E_IN_USE etc. are fine: it just stays */
            g_tm.erase_ms = now_ms() - te;
        }
        g_tm.total_ms = now_ms() - t_start;
        append_timings(out);
        log_timings(path, fn[0] ? fn : NULL);
    }
    return 200;
}
