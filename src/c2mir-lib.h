#ifndef C2MIR_LIB_H
#define C2MIR_LIB_H

#include <stddef.h>
#include "../deps/cmm/mir.h"
#include "../deps/cmm/c2mir/c2mir.h"

typedef struct rewm_compiler rewm_compiler_t;

rewm_compiler_t *rewm_compiler_create(void);
void rewm_add_include_dir(rewm_compiler_t *rc, const char *path);

/* Compile a C source file into MIR modules and load/link them.
   Returns 0 on success, -1 on error. */
int rewm_compile_file(rewm_compiler_t *rc, const char *source_path);

/* Compile C source from memory into MIR modules and load/link them.
   Returns 0 on success, -1 on error. */
int rewm_compile_string(rewm_compiler_t *rc, const char *source,
                        const char *source_name);

/* Look up a function by name and return its native code address.
   Must be called after rewm_compile_file/string.
   Returns NULL if not found. */
void *rewm_get_func(rewm_compiler_t *rc, const char *name);

/* Enable or disable MIR gen (JIT) optimization level. Default: 3. */
void rewm_set_optimize_level(rewm_compiler_t *rc, unsigned level);

/* Tear down compiler context (frees all MIR modules etc.) */
void rewm_compiler_destroy(rewm_compiler_t *rc);

/* Convenience: compile file, find function, return its address.
   Combines rewm_compile_file + rewm_get_func. */
void *rewm_compile_and_get(rewm_compiler_t *rc, const char *source_path,
                           const char *func_name);

#endif /* C2MIR_LIB_H */
