/* utf8_test.c - checks the text layer for non-ASCII names ("café.prt"). No Creo needed.
   Build + run: tests\run_utf8_test.bat
   The locale is switched to Windows-1252 first: that is the situation in which the C library's tolower/isalnum used to
   rewrite UTF-8 bytes. */
#include "../src/textutil.h"
#include <locale.h>
#include <ctype.h>

static int g_fail;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } else printf("ok:   %s\n", msg); } while (0)

/* independent validator: the OS decoder in strict mode */
static int os_valid_utf8(const char *s)
{
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0) > 0;
}

int main(void)
{
    const char cafe_u8[] = "caf\xC3\xA9.prt";           /* "café.prt" as UTF-8 */
    const wchar_t cafe_w[] = L"caf\u00e9.prt";
    char buf[256], buf2[256];
    wchar_t wbuf[256];
    Sb sb = { 0 };
    Jw jw;
    Query q;
    char loc_used[64];
    const char *loc = setlocale(LC_ALL, ".1252");
    if (!loc) loc = setlocale(LC_ALL, "English_United States.1252");
    strncpy(loc_used, loc ? loc : "(could not set; C locale)", sizeof loc_used - 1);
    loc_used[sizeof loc_used - 1] = 0;
    printf("locale for this run: %s\n\n", loc_used);

    /* 1. The cause: the locale-dependent C function used to corrupt UTF-8 */
    strcpy(buf, cafe_u8);
    { char *p; for (p = buf; *p; ++p) *p = (char)tolower((unsigned char)*p); }
    printf("(demo) old-style tolower() on UTF-8 bytes -> valid UTF-8? %s\n\n", os_valid_utf8(buf) ? "yes" : "NO (corrupted)");

    /* 2. wide -> UTF-8 */
    w2u(cafe_w, buf, sizeof buf);
    CHECK(strcmp(buf, cafe_u8) == 0, "w2u(L\"caf\\u00e9.prt\") == UTF-8 bytes 63 61 66 C3 A9");

    /* 3. ASCII-only case folding keeps UTF-8 intact */
    strcpy(buf, "CAF\xC3\xA9.PRT");
    lower_ascii(buf);
    CHECK(strcmp(buf, cafe_u8) == 0, "lower_ascii(\"CAF\\xC3\\xA9.PRT\") == \"caf\\xC3\\xA9.prt\" (only ASCII letters change)");
    CHECK(os_valid_utf8(buf), "result is valid UTF-8 even under the 1252 locale");
    strcpy(buf, cafe_u8); upper_ascii(buf);
    CHECK(os_valid_utf8(buf) && buf[0] == 'C' && (unsigned char)buf[3] == 0xC3 && (unsigned char)buf[4] == 0xA9,
          "upper_ascii changes ASCII letters only; the two UTF-8 bytes of e-acute are untouched");

    /* 4. JSON: raw UTF-8, escaping of quotes / backslashes / controls */
    jw_init(&jw, &sb);
    jw_obj(&jw, NULL);
    jw_str(&jw, "fileName", cafe_u8);
    jw_str(&jw, "path", "C:\\models\\caf\xC3\xA9\\a\"b.prt");
    jw_str(&jw, "ctl", "a\tb\nc\x01" "d");
    jw_wstr(&jw, "wide", L"na\u00efve \u4e2d\u6587 \U0001F600");
    jw_end_obj(&jw);
    printf("json: %s\n", sb.p);
    CHECK(os_valid_utf8(sb.p), "JSON body is valid UTF-8");
    CHECK(strstr(sb.p, "\"fileName\": \"caf\xC3\xA9.prt\"") != NULL, "fileName is emitted as raw UTF-8");
    CHECK(strstr(sb.p, "C:\\\\models\\\\caf\xC3\xA9\\\\a\\\"b.prt") != NULL, "backslashes and quotes are escaped");
    CHECK(strstr(sb.p, "a\\tb\\nc\\u0001d") != NULL, "control characters are escaped");
    CHECK(strstr(sb.p, "\xF0\x9F\x98\x80") != NULL, "a character outside the BMP (surrogate pair in UTF-16) survives");
    sb_free(&sb);

    /* 5. invalid input can never reach the client */
    jw_init(&jw, &sb);
    jw_obj(&jw, NULL);
    jw_str(&jw, "bad", "caf\xE9.prt");                    /* a lone Windows-1252 byte, the reported symptom */
    jw_end_obj(&jw);
    printf("json: %s\n", sb.p);
    CHECK(os_valid_utf8(sb.p), "a stray 0xE9 byte is replaced (\\ufffd), output stays valid UTF-8");
    sb_free(&sb);

    /* 6. request direction: percent-encoded UTF-8 -> bytes -> wide */
    q.query = "fileName=caf%C3%A9.prt&path=C%3A%5Cmodels%5Ccaf%C3%A9%5Ccaf%C3%A9.prt.3&sp=a+b";
    CHECK(q_get_dec(&q, "fileName", buf, sizeof buf) && strcmp(buf, cafe_u8) == 0, "q_get_dec decodes fileName=caf%C3%A9.prt");
    CHECK(q_get_dec(&q, "path", buf2, sizeof buf2) && strcmp(buf2, "C:\\models\\caf\xC3\xA9\\caf\xC3\xA9.prt.3") == 0,
          "q_get_dec decodes a non-ASCII Windows path");
    CHECK(q_get_dec(&q, "sp", buf, sizeof buf) && strcmp(buf, "a b") == 0, "'+' decodes to a space");
    CHECK(u2w(buf2, wbuf, 256) && wcsstr(wbuf, L"caf\u00e9") != NULL, "u2w turns the path into UTF-16 with U+00E9");
    w2u(wbuf, buf, sizeof buf);
    CHECK(strcmp(buf, buf2) == 0, "path round-trips UTF-8 -> UTF-16 -> UTF-8 unchanged");
    CHECK(!u2w("caf\xE9.prt", wbuf, 256), "u2w rejects invalid UTF-8 (lone 0xE9) instead of guessing");
    CHECK(utf8_valid(cafe_u8) && !utf8_valid("caf\xE9") && !utf8_valid("\xC0\x80") && !utf8_valid("\xED\xA0\x80"),
          "utf8_valid: accepts good, rejects lone byte, overlong and surrogate");

    /* 7. url_encode (agent -> browser direction) */
    url_encode(cafe_u8, buf, sizeof buf);
    CHECK(strcmp(buf, "caf%C3%A9.prt") == 0, "url_encode(\"caf\\xC3\\xA9.prt\") == \"caf%C3%A9.prt\" (not mangled by the 1252 locale)");

    /* 8. a long name is truncated on a character boundary, never emptied or split */
    { wchar_t lw[64]; int i; for (i = 0; i < 40; i++) lw[i] = L'\u00e9'; lw[40] = 0;     /* 40 x e-acute = 80 UTF-8 bytes */
      w2u(lw, buf, 11);
      CHECK(buf[0] != 0 && os_valid_utf8(buf) && strlen(buf) == 10, "w2u into a 11-byte buffer keeps 5 whole characters (valid UTF-8)"); }

    printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
