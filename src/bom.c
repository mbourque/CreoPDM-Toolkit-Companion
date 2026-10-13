/* bom.c - /bom: nested bill of materials for an assembly.
 *
 * Per assembly level: component features are rolled up by logical tip name (shaft.prt, never shaft.prt.2);
 * quantity = number of non-suppressed occurrences under ONE instance of the parent. Each unique sub-assembly
 * is expanded once (children listed under that node) if its model is already in session; nothing is retrieved.
 * Harness members are listed (type HARNESS) without loading. Depth limit BOM_MAX_DEPTH.
 */
#include "common.h"

#define BOM_MAX_DEPTH   6
#define BOM_MAX_ENTRIES 1024

typedef struct {
    char        file[200];
    const char *kind;
    int         count;
    ProMdl      mdl;         /* first in-session model seen for this name, else NULL */
} BomEntry;

typedef struct {
    BomEntry *e;
    int       n;
    int       truncated;
    int       suppressed;
} Level;

typedef struct {
    int unexpanded_not_in_memory;
    int unexpanded_depth;
    int suppressed_excluded;
    int truncated;
} BomStats;

static void bom_cb(const CompInfo *ci, void *ud)
{
    Level *lv = (Level *)ud;
    int i;
    if (ci->suppressed) { lv->suppressed++; return; }
    for (i = 0; i < lv->n; i++) {
        if (strcmp(lv->e[i].file, ci->file) == 0) {
            lv->e[i].count++;
            if (!lv->e[i].mdl && ci->mdl) lv->e[i].mdl = ci->mdl;
            return;
        }
    }
    if (lv->n >= BOM_MAX_ENTRIES) { lv->truncated = 1; return; }
    strncpy(lv->e[lv->n].file, ci->file, sizeof lv->e[lv->n].file - 1);
    lv->e[lv->n].file[sizeof lv->e[lv->n].file - 1] = 0;
    lv->e[lv->n].kind = ci->kind;
    lv->e[lv->n].count = 1;
    lv->e[lv->n].mdl = ci->mdl;
    lv->n++;
}

/* Writes the elements of a "children" array for one assembly (caller opened the array). */
static int bom_level(Jw *w, ProSolid asm_mdl, int depth, BomStats *st)
{
    Level lv;
    int i, skipped = 0, rc;

    memset(&lv, 0, sizeof lv);
    lv.e = (BomEntry *)calloc(BOM_MAX_ENTRIES, sizeof(BomEntry));
    if (!lv.e) return (int)PRO_TK_OUT_OF_MEMORY;

    rc = walk_components(asm_mdl, 0, bom_cb, &lv, &skipped);   /* one level; includes harness members */
    if (rc != PRO_TK_NO_ERROR) { free(lv.e); return rc; }
    st->suppressed_excluded += lv.suppressed;
    if (lv.truncated) st->truncated = 1;

    for (i = 0; i < lv.n; i++) {
        BomEntry *e = &lv.e[i];
        int is_asm = (strcmp(e->kind, "ASSEMBLY") == 0);

        jw_obj(w, NULL);
        jw_str(w, "filename", e->file);
        jw_int(w, "quantity", e->count);
        jw_str(w, "dependency_type", "ASSEMBLY_MEMBER");
        jw_str(w, "type", e->kind);                       /* PART | ASSEMBLY | SKELETON | HARNESS */
        if (is_asm && e->mdl && depth < BOM_MAX_DEPTH) {
            jw_arr(w, "children");
            rc = bom_level(w, (ProSolid)e->mdl, depth + 1, st);
            jw_end_arr(w);
            jw_bool(w, "expanded", 1);
            if (rc != PRO_TK_NO_ERROR) { free(lv.e); return rc; }
        } else {
            jw_arr(w, "children");
            jw_end_arr(w);
            if (is_asm) {                                  /* leaf-only sub-assembly: say why */
                jw_bool(w, "expanded", 0);
                if (!e->mdl) { jw_str(w, "leaf_reason", "not_in_memory"); st->unexpanded_not_in_memory++; }
                else         { jw_str(w, "leaf_reason", "max_depth");     st->unexpanded_depth++; }
            }
        }
        jw_end_obj(w);
    }
    free(lv.e);
    return PRO_TK_NO_ERROR;
}

void collect_bom(Sb *out, const Ctx *c, const Query *q)
{
    Sb tree = { 0 };
    Jw tw, w;
    BomStats st;
    int rc;
    (void)q;

    if (c->type != PRO_MDL_ASSEMBLY) {
        emit_errorf(out, "bom applies to ASSEMBLY; active model is %s", c->typeStr);
        return;
    }
    memset(&st, 0, sizeof st);

    jw_init(&tw, &tree);
    jw_arr(&tw, NULL);
    jw_obj(&tw, NULL);
    jw_str(&tw, "filename", c->fileName);
    jw_int(&tw, "quantity", 1);
    jw_str(&tw, "dependency_type", "ASSEMBLY_ROOT");
    jw_str(&tw, "type", "ASSEMBLY");
    jw_arr(&tw, "children");
    rc = bom_level(&tw, (ProSolid)c->mdl, 1, &st);
    jw_end_arr(&tw);
    jw_bool(&tw, "expanded", 1);
    jw_end_obj(&tw);
    jw_end_arr(&tw);
    if (rc != PRO_TK_NO_ERROR) {
        emit_errorf(out, "bom walk failed (%d)", rc);
        sb_free(&tree);
        return;
    }

    jw_init(&w, out);
    jw_obj(&w, NULL);
    jw_bool(&w, "ok", 1);
    jw_str(&w, "fileName", c->fileName);
    jw_bool(&w, "partial", st.unexpanded_not_in_memory > 0 || st.unexpanded_depth > 0 || st.truncated);
    jw_int(&w, "unexpandedNotInMemory", st.unexpanded_not_in_memory);
    jw_int(&w, "unexpandedMaxDepth", st.unexpanded_depth);
    jw_int(&w, "suppressedExcluded", st.suppressed_excluded);
    jw_int(&w, "maxDepth", BOM_MAX_DEPTH);
    jw_raw(&w, "bom", tree.p);
    jw_end_obj(&w);
    sb_puts(out, "\n");
    sb_free(&tree);
}
