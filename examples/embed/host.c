/* host.c
 * a C program that embeds embr through embr_c.h: it passes config values in, adds a function of its own, runs a
 * script and reads the results back. CTest builds and runs it, so the C interface can't rot.
 */
#include "embr_c.h"

#include <stdio.h>
#include <string.h>

static int failed = 0;

static void expect(int ok, const char* what) {
    if (!ok) { printf("FAIL: %s\n", what); failed = 1; }
}

/* scale(x, factor): a function the host adds. it checks its arguments and raises an error the script can catch */
static int scale(embr_call* c, void* userdata) {
    int* calls = (int*)userdata;
    ++*calls;
    if (embr_arg_count(c) != 2 || embr_arg_type(c, 1) == EMBR_TYPE_STRING)
        return embr_fail(c, "scale takes two numbers");
    if (embr_arg_type(c, 0) == EMBR_TYPE_INT && embr_arg_type(c, 1) == EMBR_TYPE_INT)
        embr_return_int(c, embr_arg_int(c, 0) * embr_arg_int(c, 1));
    else
        embr_return_float(c, embr_arg_float(c, 0) * embr_arg_float(c, 1));
    return 0;
}

static int greet(embr_call* c, void* userdata) {
    char buf[128];
    (void)userdata;
    snprintf(buf, sizeof buf, "hello, %s", embr_arg_string(c, 0));
    embr_return_string(c, buf);
    return 0;
}

int main(void) {
    int calls = 0;
    int64_t i;
    double d;
    const char* s;
    embr_state* e = embr_new();
    expect(e != NULL, "embr_new");

    embr_set_int(e, "width", 80);
    embr_set_float(e, "ratio", 1.5);
    embr_set_string(e, "user", "ana");
    embr_register(e, "scale", scale, &calls);
    embr_register(e, "greet", greet, NULL);

    expect(embr_run_string(e,
        "area = scale(width, 2)\n"
        "big = scale(ratio, 4)\n"
        "msg = greet(user)\n"
        "try\n"
        "    scale(1, \"x\")\n"
        "catch err\n"
        "    caught = err[\"message\"]\n"
        "end\n", "config") == 0, embr_last_error(e));

    expect(embr_get_int(e, "area", &i) == 0 && i == 160, "area is 160");
    expect(embr_get_float(e, "big", &d) == 0 && d == 6.0, "big is 6.0");
    expect(embr_get_string(e, "msg", &s) == 0 && strcmp(s, "hello, ana") == 0, "msg");
    expect(embr_get_string(e, "caught", &s) == 0 && strcmp(s, "scale takes two numbers") == 0, "the host's error reached the script");
    expect(calls == 3, "scale was called three times");
    expect(embr_get_type(e, "area") == EMBR_TYPE_INT && embr_get_type(e, "nope") == EMBR_TYPE_NONE, "embr_get_type");
    expect(embr_get_int(e, "msg", &i) == 1, "a string is not an int");

    /* names stay between runs */
    expect(embr_run_string(e, "total = area + 1\n", "second") == 0 && embr_get_int(e, "total", &i) == 0 && i == 161, "names stay between runs");

    /* an error comes back as a code and a plain message */
    expect(embr_run_string(e, "x = 1\ny = x +\n", "broken") == 1, "a parse error is reported");
    expect(strstr(embr_last_error(e), "broken:") == embr_last_error(e), embr_last_error(e));
    expect(embr_run_string(e, "xs = [1]\nv = xs[5]\n", "raises") == 1 && strstr(embr_last_error(e), "raises:2:") != NULL, embr_last_error(e));
    expect(embr_run_string(e, "ok = 1\n", "fine") == 0 && embr_last_error(e)[0] == 0, "a good run clears the error");

    /* sandbox: only the plugins on the list can be imported */
    {
        embr_state* box = embr_new();
        const char* allowed[] = { "str" };
        embr_allow_plugins(box, allowed, 1);
        embr_allow_file_io(box, 0);
        expect(embr_run_string(box, "import \"str\"\nu = str_upper(\"ok\")\n", "box") == 0, embr_last_error(box));
        expect(embr_get_string(box, "u", &s) == 0 && strcmp(s, "OK") == 0, "an allowed plugin works");
        expect(embr_run_string(box, "import \"os\"\n", "box") == 1 && strstr(embr_last_error(box), "not allowed") != NULL, embr_last_error(box));
        embr_free(box);
    }

    /* capabilities: plugins that need a denied one are refused whatever their name, the others load */
    {
        embr_state* box = embr_new();
        const char* s;
        expect(embr_deny_capabilities(box, "bogus") == 1, "an unknown capability name is refused");
        expect(embr_deny_capabilities(box, "network,process") == 0, "a list of capabilities is accepted");
        expect(embr_run_string(box, "import \"str\"\nu = str_upper(\"ok\")\n", "box") == 0, embr_last_error(box));
        expect(embr_get_string(box, "u", &s) == 0 && strcmp(s, "OK") == 0, "a plugin that needs nothing works");
        expect(embr_run_string(box, "import \"fs\"\n", "box") == 0, "denying network and process leaves the filesystem");
        expect(embr_run_string(box, "import \"os\"\n", "box") == 1 && strstr(embr_last_error(box), "it needs process") != NULL, embr_last_error(box));
        expect(embr_run_string(box, "import \"http\"\n", "box") == 1 && strstr(embr_last_error(box), "it needs network") != NULL, embr_last_error(box));
        embr_free(box);
    }

    embr_free(e);
    if (!failed) printf("embed host OK\n");
    return failed;
}
