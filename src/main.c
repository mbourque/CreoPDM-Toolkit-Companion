/* main.c - Toolkit entry points + ribbon command */
#include "common.h"
#include <ProMenuBar.h>
#include <ProUICmd.h>
#include <ProMessage.h>

/* Ribbon command: runs the same /structure route and writes it to a file.
   Output: env CREOPDM_STRUCTURE_OUT, else %TEMP%\creopdm_structure.json */
static void export_structure_file(void)
{
    char path[MAX_PATH * 2];
    Sb out = { 0 };
    FILE *f;
    DWORD n = GetEnvironmentVariableA("CREOPDM_STRUCTURE_OUT", path, sizeof path);
    if (n == 0 || n >= sizeof path) {
        char tmp[MAX_PATH];
        DWORD t = GetTempPathA(sizeof tmp, tmp);
        if (t == 0 || t >= sizeof tmp) strcpy(tmp, ".\\");
        _snprintf(path, sizeof path, "%screopdm_structure.json", tmp);
        path[sizeof path - 1] = 0;
    }
    route_request("GET", "/structure", "", &out);
    f = fopen(path, "wb");
    if (f) { fwrite(out.p, 1, out.len, f); fclose(f); }
    sb_free(&out);
}

static ProError cmd_action(uiCmdCmdId command, uiCmdValue *value, void *data)
{
    (void)command; (void)value; (void)data;
    export_structure_file();
    return PRO_TK_NO_ERROR;
}
static uiCmdAccessState cmd_access(uiCmdAccessMode mode)
{
    (void)mode;
    return ACCESS_AVAILABLE;
}

int user_initialize(int argc, char *argv[], char *version, char *build, wchar_t errbuf[80])
{
    uiCmdCmdId id;
    ProError rc;
    (void)argc; (void)argv; (void)version; (void)build;

    rc = ProCmdActionAdd("creopdm_structure_export", (uiCmdCmdActFn)cmd_action,
                         uiProe2ndImmediate, cmd_access, PRO_B_TRUE, PRO_B_TRUE, &id);
    if (rc != PRO_TK_NO_ERROR) {
        ProStringToWstring(errbuf, "CreoPDM: ProCmdActionAdd failed");
        return -1;
    }
    ProCmdDesignate(id, "CPDM_LABEL", "CPDM_HELP", "CPDM_DESC", L"creopdm_msg.txt");

    /* HTTP is best-effort: failure must never stop the app from loading. */
    if (http_start()) ProMessageDisplay(L"creopdm_msg.txt", "CPDM_HTTP_UP");
    else              ProMessageDisplay(L"creopdm_msg.txt", "CPDM_HTTP_FAIL");
    return 0;
}

void user_terminate(void) { http_stop(); }
