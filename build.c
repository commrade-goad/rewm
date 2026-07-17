/* build.cmm — rewm build script
 *
 * Compiles:
 *   host.c          (main binary)
 *   c2mir-lib.c     (wrapper)
 *   mir.c / mir-gen.c / c2mir/c2mir.c  (MIR + c2mir core)
 *
 * Also copies c2mir headers as needed.
 */
#define NOB_IMPLEMENTATION
#include "nob.h"

#ifndef CC
#define CC "gcc"
#endif

const char *cflags[] = {
    "-I.", "-Isrc", "-Ideps/cmm", "-Ideps/cmm/c2mir",
    "-MMD", "-MP", "-g",
    "-std=gnu11", "-O3",
    "-DNDEBUG", "-DC2MIR_PARALLEL",
};
const int ncflags = sizeof cflags / sizeof cflags[0];

const char *ldflags[] = {"-lm", "-ldl", "-lpthread", "-lX11", "-lXinerama"};
const int nldflags = sizeof ldflags / sizeof ldflags[0];

const char *srcs[] = {
    "src/host.c",
    "src/c2mir-lib.c",
    "deps/cmm/mir.c",
    "deps/cmm/mir-gen.c",
    "deps/cmm/c2mir.c",
};
const int nsrcs = sizeof srcs / sizeof srcs[0];

const char *exe = "rewm";

int main(int argc, char **argv) {
    NOB_GO_REBUILD_URSELF(argc, argv);
    nob_shift(argv, argc);

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "clean") == 0) {
            for (int j = 0; j < nsrcs; j++) {
                char *obj = nob_temp_sprintf("%s.o", srcs[j]);
                nob_delete_file(obj);
            }
            nob_delete_file(exe);
            return 0;
        }
    }

    const char *objs[nsrcs];

    Nob_Cmd cmd = {0};
    int rebuild_p = 0;

    for (int i = 0; i < nsrcs; i++) {
        const char *obj = nob_temp_sprintf("%s.o", srcs[i]);
        objs[i] = obj;

        if (!nob_needs_rebuild1(obj, srcs[i]))
            continue;

        nob_cmd_append(&cmd, CC);
        for (int j = 0; j < ncflags; j++)
            nob_cmd_append(&cmd, cflags[j]);
        nob_cmd_append(&cmd, "-c", "-o", obj, srcs[i]);
        if (!nob_cmd_run(&cmd)) return 1;
        rebuild_p = 1;
    }

    if (!nob_needs_rebuild(exe, objs, nsrcs))
        return 0;

    nob_cmd_append(&cmd, CC, "-o", exe);
    for (int i = 0; i < nsrcs; i++)
        nob_cmd_append(&cmd, objs[i]);
    for (int j = 0; j < nldflags; j++)
        nob_cmd_append(&cmd, ldflags[j]);
    if (!nob_cmd_run(&cmd)) return 1;

    nob_log(NOB_INFO, "built %s", exe);
    return 0;
}
