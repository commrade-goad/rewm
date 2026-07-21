#include "c2mir-lib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <assert.h>

#include "mir-gen.h"

/* ---- helper: read file into heap buffer --------------------------------- */
static uint8_t *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[n] = '\0';
    *len_out = n;
    return buf;
}

/* ---- getc callback for c2mir_compile (reads from our buffer) ------------ */
static int buf_getc(void *data) {
    struct { const uint8_t *buf; size_t len; size_t pos; } *s = data;
    return s->pos < s->len ? s->buf[s->pos++] : EOF;
}

/* ---- import resolver -----------------------------------------------------
   Called by MIR_link to resolve external C symbols (Xlib, libc, etc.).
   We search RTLD_DEFAULT which picks up everything linked into host. */
static void *import_resolver(const char *name) {
    void *sym = dlsym(RTLD_DEFAULT, name);
    if (sym) return sym;
    /* Known builtins MIR needs: */
    if (strcmp(name, "abort") == 0) return abort;
    if (strcmp(name, "dlsym") == 0) return dlsym;
    if (strcmp(name, "dlopen") == 0) return dlopen;
    if (strcmp(name, "dlclose") == 0) return dlclose;
    if (strcmp(name, "dlerror") == 0) return dlerror;
    if (strcmp(name, "stat") == 0) return stat;
    if (strcmp(name, "lstat") == 0) return lstat;
    fprintf(stderr, "rewm: import resolver: can't find symbol '%s'\n", name);
    return NULL;
}

/* ---- compiler context ---------------------------------------------------- */
struct rewm_compiler {
    MIR_context_t ctx;
    int gen_inited;
    unsigned optimize_level;
    const char **include_dirs;
    size_t include_dirs_num;
    size_t include_dirs_cap;
};

rewm_compiler_t *rewm_compiler_create(void) {
    rewm_compiler_t *rc = calloc(1, sizeof(*rc));
    if (!rc) return NULL;
    rc->ctx = MIR_init();
    if (!rc->ctx) { free(rc); return NULL; }
    c2mir_init(rc->ctx);
    rc->optimize_level = 3;
    rc->include_dirs = NULL;
    rc->include_dirs_num = 0;
    rc->include_dirs_cap = 0;
    return rc;
}

void rewm_add_include_dir(rewm_compiler_t *rc, const char *path) {
    if (!rc || !path) return;
    if (rc->include_dirs_num >= rc->include_dirs_cap) {
        size_t new_cap = rc->include_dirs_cap ? rc->include_dirs_cap * 2 : 8;
        const char **new_dirs = realloc(rc->include_dirs, new_cap * sizeof(const char *));
        if (!new_dirs) return;
        rc->include_dirs = new_dirs;
        rc->include_dirs_cap = new_cap;
    }
    rc->include_dirs[rc->include_dirs_num++] = path;
}

void rewm_compiler_destroy(rewm_compiler_t *rc) {
    if (!rc) return;
    if (rc->gen_inited) MIR_gen_finish(rc->ctx);
    c2mir_finish(rc->ctx);
    MIR_finish(rc->ctx);
    for (size_t i = 0; i < rc->include_dirs_num; i++) {
        free((void *)rc->include_dirs[i]);
    }
    free(rc->include_dirs);
    free(rc);
}

void rewm_set_optimize_level(rewm_compiler_t *rc, unsigned level) {
    rc->optimize_level = level;
}

/* ---- compile source ------------------------------------------------------*/
static int compile_source(rewm_compiler_t *rc,
                          const uint8_t *code, size_t code_len,
                          const char *source_name,
                          int is_preprocessed) {
    struct c2mir_options opts;
    memset(&opts, 0, sizeof(opts));
    opts.message_file      = stderr;
    opts.no_prepro_p       = is_preprocessed ? 1 : 0;
    /* compile into MIR modules (no output files) */
    opts.asm_p         = 0;
    opts.object_p      = 0;
    
    /* Pass include directories to c2mir */
    opts.include_dirs = rc->include_dirs;
    opts.include_dirs_num = rc->include_dirs_num;

    struct { const uint8_t *buf; size_t len; size_t pos; } buf_state;
    buf_state.buf = code;
    buf_state.len = code_len;
    buf_state.pos = 0;

    /* c2mir_compile reads from getc callback, populates MIR context modules */
    int ok = c2mir_compile(rc->ctx, &opts, buf_getc, &buf_state,
                           source_name, /* output_file = */ NULL);
    return ok ? 0 : -1;
}

int rewm_compile_file(rewm_compiler_t *rc, const char *source_path) {
    size_t len;
    uint8_t *code = read_file(source_path, &len);
    if (!code) {
        fprintf(stderr, "rewm: cannot read '%s'\n", source_path);
        return -1;
    }
    int ret = compile_source(rc, code, len, source_path, 0);
    free(code);
    return ret;
}

int rewm_compile_string(rewm_compiler_t *rc, const char *source,
                        const char *source_name) {
    return compile_source(rc, (const uint8_t *)source, strlen(source),
                          source_name, 0);
}

/* ---- load modules & link via JIT (MIR_gen) ------------------------------ */
static int ensure_linked(rewm_compiler_t *rc) {
    if (rc->gen_inited) return 0; /* already linked */

    DLIST(MIR_module_t) *mlist = MIR_get_module_list(rc->ctx);
    MIR_module_t m;

    /* Load every module */
    for (m = DLIST_HEAD(MIR_module_t, *mlist);
         m != NULL; m = DLIST_NEXT(MIR_module_t, m)) {
        MIR_load_module(rc->ctx, m);
    }

    /* Init generator & link */
    MIR_gen_init(rc->ctx);
    if (rc->optimize_level > 0)
        MIR_gen_set_optimize_level(rc->ctx, rc->optimize_level);

    MIR_link(rc->ctx, MIR_set_gen_interface, import_resolver);

    rc->gen_inited = 1;
    return 0;
}

/* ---- find a function by name and return its native address --------------- */
void *rewm_get_func(rewm_compiler_t *rc, const char *name) {
    if (ensure_linked(rc) != 0) return NULL;

    DLIST(MIR_module_t) *mlist = MIR_get_module_list(rc->ctx);
    for (MIR_module_t m = DLIST_HEAD(MIR_module_t, *mlist);
         m != NULL; m = DLIST_NEXT(MIR_module_t, m)) {
        for (MIR_item_t func = DLIST_HEAD(MIR_item_t, m->items);
             func != NULL; func = DLIST_NEXT(MIR_item_t, func)) {
            if (func->item_type == MIR_func_item
                && strcmp(func->u.func->name, name) == 0) {
                return func->addr;
            }
        }
    }
    return NULL;
}

void *rewm_compile_and_get(rewm_compiler_t *rc, const char *source_path,
                           const char *func_name) {
    if (rewm_compile_file(rc, source_path) != 0) return NULL;
    return rewm_get_func(rc, func_name);
}
