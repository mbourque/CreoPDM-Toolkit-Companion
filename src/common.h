/* common.h - shared types for the CreoPDM Toolkit service */
#ifndef CPDM_COMMON_H
#define CPDM_COMMON_H

#define PRO_USE_VAR_ARGS
#include <winsock2.h>
#include <ws2tcpip.h>

#include <ProToolkit.h>
#include <ProMdl.h>
#include <ProSolid.h>
#include <ProUtil.h>
#include <wchar.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <windows.h>
#include "textutil.h"

#define CPDM_SERVICE  "creopdm-toolkit"
#define CPDM_VERSION  "0.4.1"

/* Sb, Jw (JSON writer), w2u/u2w, ASCII helpers and query parsing live in textutil.h (no Toolkit dependency). */

/* ---------- helpers ---------- */
int   is_noname_u(const char *u);                    /* "no_name" (any case): Creo placeholder for unnamed hidden features */
int   file_is_harness_w(const wchar_t *wpath);       /* first line of the file says PART/HARNESS_PART */
void  logf_(const char *fmt, ...);                   /* %TEMP%\creopdm_toolkit.log */
void  emit_error(Sb *out, const char *msg);          /* resets *out to {"ok":false,...} */
void  emit_errorf(Sb *out, const char *fmt, ...);

/* ---------- request / model context ---------- */

typedef struct {
    int         has;            /* a model is current */
    ProMdl      mdl;
    ProMdlType  type;
    char        fileName[256];  /* logical, lowercase, e.g. tdd-demo.asm */
    const char *typeStr;        /* PART|ASSEMBLY|DRAWING|MFG|LAYOUT|... */
} Ctx;
int  ctx_load(Ctx *c);          /* fills from the current model */
int  ctx_for_model(Ctx *c, ProMdl mdl);   /* fills from a given model handle */
void ctx_fallback_name(Ctx *c, const char *requested);   /* use requested tip if the handle has no name */
const char *mdl_type_str(ProMdlType t);

/* ---------- collectors: each writes a complete JSON object into *out ---------- */
typedef void (*CollectFn)(Sb *out, const Ctx *c, const Query *q);

void collect_identity  (Sb *out, const Ctx *c, const Query *q);
void collect_units     (Sb *out, const Ctx *c, const Query *q);
void collect_parameters(Sb *out, const Ctx *c, const Query *q);
void collect_mass      (Sb *out, const Ctx *c, const Query *q);
void collect_materials (Sb *out, const Ctx *c, const Query *q);
void collect_features  (Sb *out, const Ctx *c, const Query *q);
void collect_dimensions(Sb *out, const Ctx *c, const Query *q);
void collect_relations (Sb *out, const Ctx *c, const Query *q);
void collect_structure (Sb *out, const Ctx *c, const Query *q);
void collect_constraints(Sb *out, const Ctx *c, const Query *q);
void collect_simpreps  (Sb *out, const Ctx *c, const Query *q);
void collect_drawing   (Sb *out, const Ctx *c, const Query *q);
void collect_mfg       (Sb *out, const Ctx *c, const Query *q);
void collect_bom       (Sb *out, const Ctx *c, const Query *q);
void collect_harness_probe(Sb *out, const Ctx *c, const Query *q);

/* ---------- component walker (structure + constraints) ---------- */
#define WALK_MAX_DEPTH 16
typedef struct {
    int         id;             /* feature id in its owner assembly */
    int         level;          /* 1 = top level */
    int         path[WALK_MAX_DEPTH + 1];  /* ids from root, incl. this component */
    int         pathLen;
    ProFeature *feat;
    ProSolid    owner;          /* assembly that owns the feature */
    ProMdl      mdl;            /* referenced model if in session, else NULL */
    char        file[256];      /* logical file name, lowercase */
    const char *kind;           /* PART|ASSEMBLY|SKELETON */
    int         skel;           /* 1 skeleton, 0 not, -1 unknown (model not in memory) */
    int         harness;        /* cabling harness member */
    int         synthetic;      /* row built from the harness list; feat is NULL */
    char        featName[128];  /* "" if none */
    const char *status;
    int         suppressed;
} CompInfo;
typedef void (*CompCb)(const CompInfo *ci, void *ud);
/* Returns 0 on success, else a ProError code. skipped counts unresolvable comps. */
int walk_components(ProSolid root, int recurse, CompCb cb, void *ud, int *skipped);

/* ---------- router ---------- */
/* Runs on Creo's main thread. Fills *out with a complete JSON document. */
int  route_request(const char *method, const char *path, const char *query, Sb *out);  /* HTTP status */

/* ---------- http server (http_server.c) ---------- */
int  http_start(void);          /* main thread */
void http_stop(void);

#endif
