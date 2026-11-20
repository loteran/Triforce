/* What the game was built with against the mods folder (host/mod_code.h). tests/test_mod_code.py compiles and runs it:
   alone for the reader's own checks, and with a record nx2/code_mods.py wrote and the patch files of a mods folder,
     mod_code_test <record> [<path under the mods folder> <file>]...
   to print what the game's start would say of each: "<state> <path> | <why>", then "gone <path>" for each patch that is
   built in and was not among them. */
#include "mod_code.h"
#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static char *whole(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *data = (char *)malloc((1 << 20) + 1);
    *len = fread(data, 1, 1 << 20, f);
    fclose(f);
    data[*len] = 0;
    return data;
}

static void reader(void) {
    CHECK(mod_code_is_patch("a.ips") && mod_code_is_patch("E0F3CCFA.IPS") && mod_code_is_patch("60 fps.pchtxt") && mod_code_is_patch("x.PchTxt"));
    CHECK(!mod_code_is_patch(".ips") && !mod_code_is_patch("ips") && !mod_code_is_patch("a.ips.bak") && !mod_code_is_patch("a.txt") &&
          !mod_code_is_patch("") && !mod_code_is_patch("pchtxt") && !mod_code_is_patch("a.ipsx") && !mod_code_is_patch("a_ips"));
    CHECK(mod_code_fnv("", 0) == 0xcbf29ce484222325ull && mod_code_fnv("a", 1) == 0xaf63dc4c8601ec8cull && mod_code_fnv("foobar", 6) == 0x85944171f73967e8ull);

    char text[] =
        "# a note\tapplied\t1\t2\n"
        "applied\t00000000000000ff\t12\tfps/exefs/a.ips\tmain\t8\r\n"
        "refused\tcbf29ce484222325\t0\told/exefs/b.pchtxt\tit is for build 0123, which is not this game's code\n"
        "\n"
        "applied\tnot hex\t12\tx/exefs/c.ips\tmain\t8\n"
        "applied\t00ff\ttwelve\tx/exefs/d.ips\tmain\t8\n"
        "applied\t00ff\t12\t\tmain\t8\n"
        "applied\t00ff\t12\n"
        "built\t00ff\t12\tx/exefs/e.ips\tmain\t8\n"
        "applied\t0000000000000001\t3\tlast/exefs/f.ips\tsdk\t1";   /* (no line end after the last) */
    ModCodeEntry e[8];
    const int n = mod_code_parse(text, e, 8);
    CHECK(n == 3);
    CHECK(e[0].applied && e[0].fnv == 0xff && e[0].size == 12 && !strcmp(e[0].path, "fps/exefs/a.ips") && !e[0].why[0] && !e[0].seen);
    CHECK(!e[1].applied && e[1].fnv == 0xcbf29ce484222325ull && e[1].size == 0 && !strcmp(e[1].path, "old/exefs/b.pchtxt"));
    CHECK(!strcmp(e[1].why, "it is for build 0123, which is not this game's code"));
    CHECK(e[2].applied && e[2].fnv == 1 && e[2].size == 3 && !strcmp(e[2].path, "last/exefs/f.ips"));

    const char *why = "?";
    CHECK(mod_code_state(e, n, "fps/exefs/a.ips", 0xff, 12, &why) == MOD_CODE_BUILT && !why[0] && e[0].seen);
    CHECK(mod_code_state(e, n, "fps/exefs/a.ips", 0xfe, 12, &why) == MOD_CODE_CHANGED && mod_code_state(e, n, "fps/exefs/a.ips", 0xff, 13, &why) == MOD_CODE_CHANGED);
    CHECK(mod_code_state(e, n, "old/exefs/b.pchtxt", 0xcbf29ce484222325ull, 0, &why) == MOD_CODE_REFUSED && strstr(why, "build 0123") && e[1].seen);
    CHECK(mod_code_state(e, n, "old/exefs/b.pchtxt", 5, 9, &why) == MOD_CODE_WAITS && !why[0]);   /* (edited since it was refused) */
    CHECK(mod_code_state(e, n, "new/exefs/n.ips", 0xff, 12, &why) == MOD_CODE_WAITS && mod_code_state(e, n, "FPS/exefs/a.ips", 0xff, 12, NULL) == MOD_CODE_WAITS);
    int at = 0;
    const ModCodeEntry *gone = mod_code_gone(e, n, &at);
    CHECK(gone == &e[2] && mod_code_gone(e, n, &at) == NULL && mod_code_gone(e, n, &at) == NULL);   /* (the refused one is not "gone") */
    CHECK(mod_code_state(NULL, 0, "a/exefs/b.ips", 1, 1, &why) == MOD_CODE_WAITS);   /* (a game built without a record) */
    at = 0;
    CHECK(mod_code_gone(NULL, 0, &at) == NULL);

    char many[] = "applied\t1\t1\ta\tmain\t1\napplied\t2\t1\tb\tmain\t1\napplied\t3\t1\tc\tmain\t1\n";   /* (no more than there is room for) */
    CHECK(mod_code_parse(many, e, 2) == 2 && !strcmp(e[1].path, "b"));
    char longer[MOD_CODE_PATH + 200];   /* (a path longer than an entry holds: passed over, not cut) */
    int k = snprintf(longer, sizeof longer, "applied\t1\t1\t");
    memset(longer + k, 'p', MOD_CODE_PATH + 20), k += MOD_CODE_PATH + 20;
    snprintf(longer + k, sizeof longer - (size_t)k, "\tmain\t1\napplied\t2\t1\tok\tmain\t1\n");
    CHECK(mod_code_parse(longer, e, 8) == 1 && !strcmp(e[0].path, "ok"));
    char reason[MOD_CODE_WHY + 200];   /* (a reason longer than an entry holds: cut) */
    k = snprintf(reason, sizeof reason, "refused\t1\t1\tr\t");
    memset(reason + k, 'w', MOD_CODE_WHY + 50), reason[k + MOD_CODE_WHY + 50] = 0;
    CHECK(mod_code_parse(reason, e, 8) == 1 && strlen(e[0].why) == MOD_CODE_WHY - 1);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        reader();
        if (!fails) printf("PASS\n");
        return fails != 0;
    }
    size_t len = 0;
    char *text = whole(argv[1], &len);
    static ModCodeEntry e[64];
    const int n = text ? mod_code_parse(text, e, 64) : 0;
    for (int i = 2; i + 1 < argc; i += 2) {
        char *data = whole(argv[i + 1], &len);
        const char *why = "";
        const int state = data ? mod_code_state(e, n, argv[i], mod_code_fnv(data, len), len, &why) : -1;
        printf("%d %s | %s\n", state, argv[i], why);
        free(data);
    }
    int at = 0;
    for (const ModCodeEntry *x; (x = mod_code_gone(e, n, &at)) != NULL;) printf("gone %s\n", x->path);
    free(text);
    return 0;
}
