/* main.c - Toolkit entry points + ribbon commands */
#include "common.h"
#include <ProMenuBar.h>
#include <ProUICmd.h>
#include <ProMessage.h>
#include <ProWindows.h>
#include <ProNavigator.h>

#define MSGFILE L"creopdm_msg.txt"

/* ---------- CreoPDM tab commands (experimental) ----------
   "CreoPDM"        (creopdm_app)    shows the full CreoPDM web UI in Creo's EMBEDDED browser (URL from env CREOPDM_URL)
                                     and sets how much of the Creo window it covers (env CREOPDM_BROWSER_PERCENT, default 50).
   "CreoPDM Browse" (creopdm_browse) shows the compact CreoPDM file explorer for the current product in a NAVIGATOR pane
                                     (URL from env CREOPDM_EXPLORER_URL, else CREOPDM_URL + "embed/explorer"), passing the
                                     active model as context (?file=<name>&folder=<folder>) so CreoPDM can pick the product.
   The tab itself is laid out in Creo
   (File > Options > Customize Ribbon > TOOLKIT Commands) and saved to toolkitribbonui.rbn. */

/* "CreoPDM": the full web UI in the embedded browser, widened */
static ProError act_app(uiCmdCmdId command, uiCmdValue *value, void *data)
{
    char url[1024], pct[16];
    wchar_t wurl[1024];
    double percent = 50.0;
    int win = 0;
    ProError rc, rs;
    int have_url = getenv_utf8("CREOPDM_URL", url, sizeof url) && url[0];
    (void)command; (void)value; (void)data;

    if (!have_url) {
        ProMessageDisplay(MSGFILE, "CPDM_NO_URL");
        logf_("CreoPDM: CREOPDM_URL is not set");
        return PRO_TK_NO_ERROR;
    }
    if (GetEnvironmentVariableA("CREOPDM_BROWSER_PERCENT", pct, sizeof pct) > 0) {
        double p = atof(pct);
        if (p >= 10.0 && p <= 95.0) percent = p;
        else logf_("CreoPDM: ignoring CREOPDM_BROWSER_PERCENT '%s' (use 10..95)", pct);
    }
    if (!u2w(url, wurl, 1024)) { logf_("CreoPDM: CREOPDM_URL is not valid text"); ProMessageDisplay(MSGFILE, "CPDM_APP_FAIL"); return PRO_TK_NO_ERROR; }

    rc = ProWindowCurrentGet(&win);
    if (rc == PRO_TK_NO_ERROR) rc = ProWindowURLShow(win, wurl);          /* load the page in the embedded browser */
    logf_("CreoPDM: ProWindowURLShow(win %d, %s) -> %d", win, url, (int)rc);
    if (rc != PRO_TK_NO_ERROR) { ProMessageDisplay(MSGFILE, "CPDM_APP_FAIL"); return PRO_TK_NO_ERROR; }

    rs = ProWindowBrowserSizeSet(win, percent);                            /* widen it (not applicable if the browser is in a separate window) */
    logf_("CreoPDM: ProWindowBrowserSizeSet(win %d, %.0f%%) -> %d", win, percent, (int)rs);
    return PRO_TK_NO_ERROR;
}

/* "CreoPDM Browse": compact file explorer in a navigator pane, with the active model as context */
static int g_pane_added;

/* quiet: automatic start-up attempt (no user messages; returns nonzero when the pane exists) */
static int browse_show(int quiet)
{
    char base[1024], url[2048], q[1400];
    wchar_t wurl[2048];
    size_t bl;
    Ctx c;
    ProError rc;

    if (!getenv_utf8("CREOPDM_EXPLORER_URL", base, sizeof base) || !base[0]) {                 /* default: CREOPDM_URL + "embed/explorer" */
        if (!getenv_utf8("CREOPDM_URL", base, sizeof base - 32) || !base[0]) {
            if (!quiet) ProMessageDisplay(MSGFILE, "CPDM_NO_URL");
            logf_("Browse: CREOPDM_URL / CREOPDM_EXPLORER_URL is not set");
            return -1;
        }
        bl = strlen(base);
        if (bl && base[bl - 1] != '/') strcat(base, "/");
        strcat(base, "embed/explorer");
    }

    /* context: the active model's file name and folder, so CreoPDM can pick the product */
    q[0] = 0;
    if (ctx_load(&c) && c.fileName[0]) {
        char fe[600], fo[900], folder[MAX_PATH * 2];
        ProPath origin;
        char *slash;
        origin[0] = 0;
        folder[0] = 0;
        if (ProMdlOriginGet(c.mdl, origin) == PRO_TK_NO_ERROR && origin[0]) {
            w2u(origin, folder, sizeof folder);
            slash = strrchr(folder, '\\');
            if (!slash) slash = strrchr(folder, '/');
            if (slash) *slash = 0; else folder[0] = 0;
        }
        url_encode(c.fileName, fe, sizeof fe);
        url_encode(folder, fo, sizeof fo);
        _snprintf(q, sizeof q, "%cfile=%s&folder=%s", strchr(base, '?') ? '&' : '?', fe, fo);
        q[sizeof q - 1] = 0;
    }
    _snprintf(url, sizeof url, "%s%s", base, q);
    url[sizeof url - 1] = 0;
    if (!u2w(url, wurl, 2048)) { logf_("Browse: the explorer address is not valid text"); if (!quiet) ProMessageDisplay(MSGFILE, "CPDM_BROWSE_FAIL"); return -1; }

    if (!g_pane_added) {
        rc = ProNavigatorpaneBrowserAdd((char *)"CreoPDM", NULL, wurl);
        if (rc == PRO_TK_NO_ERROR) g_pane_added = 1;
        logf_("Browse: ProNavigatorpaneBrowserAdd(%s) -> %d", url, (int)rc);
    } else {
        rc = ProNavigatorpaneBrowserURLSet((char *)"CreoPDM", wurl);
        logf_("Browse: ProNavigatorpaneBrowserURLSet(%s) -> %d", url, (int)rc);
    }
    if (rc != PRO_TK_NO_ERROR && !quiet) ProMessageDisplay(MSGFILE, "CPDM_BROWSE_FAIL");
    return rc == PRO_TK_NO_ERROR;
}

static ProError act_browse(uiCmdCmdId command, uiCmdValue *value, void *data)
{
    (void)command; (void)value; (void)data;
    browse_show(0);
    return PRO_TK_NO_ERROR;
}

/* Start-up: add the navigator pane without a click. The navigator may not exist yet while user_initialize runs, so retry on a
   timer (main thread) every 2 s, up to 30 times. CREOPDM_AUTO_NAV=0 turns this off. */
static UINT_PTR g_auto_timer;
static int      g_auto_tries;

static VOID CALLBACK auto_nav_tick(HWND h, UINT m, UINT_PTR id, DWORD t)
{
    int r;
    (void)h; (void)m; (void)t;
    r = browse_show(1);
    if (r != 0 || ++g_auto_tries >= 30) {                    /* done, not configured (-1), or out of tries */
        KillTimer(NULL, id);
        g_auto_timer = 0;
        logf_("Auto navigator: %s after %d attempt(s)", r > 0 ? "added" : "gave up", g_auto_tries + 1);
    }
}

static void auto_nav_start(void)
{
    char v[8];
    if (GetEnvironmentVariableA("CREOPDM_AUTO_NAV", v, sizeof v) > 0 && v[0] == '0') return;
    g_auto_timer = SetTimer(NULL, 0, 2000, auto_nav_tick);
    if (!g_auto_timer) logf_("Auto navigator: SetTimer failed (%lu)", GetLastError());
}


static uiCmdAccessState cmd_access(uiCmdAccessMode mode)
{
    (void)mode;
    return ACCESS_AVAILABLE;
}

/* register one command and make it placeable in Customize Ribbon (label/help/desc are message keys) */
static ProError add_cmd(const char *name, uiCmdCmdActFn fn, const char *label, const char *help, const char *desc)
{
    uiCmdCmdId id;
    ProError rc = ProCmdActionAdd((char *)name, fn, uiProe2ndImmediate, cmd_access,
                                  PRO_B_TRUE, PRO_B_TRUE, &id);
    if (rc != PRO_TK_NO_ERROR) { logf_("ProCmdActionAdd(%s) failed: %d", name, (int)rc); return rc; }
    ProCmdDesignate(id, (char *)label, (char *)help, (char *)desc, MSGFILE);
    return PRO_TK_NO_ERROR;
}

int user_initialize(int argc, char *argv[], char *version, char *build, wchar_t errbuf[80])
{
    (void)argc; (void)argv; (void)build; (void)errbuf;
    if (version) { strncpy(g_creo_version, version, sizeof g_creo_version - 1); g_creo_version[sizeof g_creo_version - 1] = 0; }

    /* tab commands: failures are logged but never stop the app from loading */
    add_cmd("creopdm_app",      (uiCmdCmdActFn)act_app,    "CPDM_APP_LABEL",      "CPDM_APP_HELP",      "CPDM_APP_DESC");
    add_cmd("creopdm_browse",   (uiCmdCmdActFn)act_browse, "CPDM_BROWSE_LABEL",   "CPDM_BROWSE_HELP",   "CPDM_BROWSE_DESC");

    auto_nav_start();

    /* HTTP is best-effort: failure must never stop the app from loading. */
    if (http_start()) ProMessageDisplay(MSGFILE, "CPDM_HTTP_UP");
    else              ProMessageDisplay(MSGFILE, "CPDM_HTTP_FAIL");
    return 0;
}

void user_terminate(void)
{
    if (g_auto_timer) { KillTimer(NULL, g_auto_timer); g_auto_timer = 0; }
    http_stop();
}
