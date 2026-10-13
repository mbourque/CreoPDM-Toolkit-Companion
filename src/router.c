/* router.c - maps paths to collectors; /health and /collect. Runs on Creo's main thread. */
#include "common.h"

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
};
#define NDOM ((int)(sizeof DOMAINS / sizeof DOMAINS[0]))

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
            DOMAINS[i].fn(&r, c, q);
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
                    d->fn(&r, c, q);      /* collector reports its own not-applicable error */
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

    if (strcmp(path, "/collect") == 0) {
        if (!c.has) { emit_error(out, "No active model in session"); return 200; }
        do_collect(out, &c, &q);
        return 200;
    }

    d = (path[0] == '/') ? find_domain(path + 1) : NULL;
    if (!d) { emit_error(out, "not found"); return 404; }
    if (!c.has) { emit_error(out, "No active model in session"); return 200; }
    d->fn(out, &c, &q);
    return 200;
}
