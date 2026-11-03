// CI stand-in for gen/table.c: no guest functions, no imports.
#include "../runtime/rt.h"
#include "funcs.h"
static void f_none(void) { rt_fatal("stub build: no recompiled game code"); }
const struct fentry FUNCS[] = { { 0, f_none } };
const int NFUNCS = 0;
const uint32_t GUEST_ENTRY = 0;
const struct import_entry IMPORTS[] = { { 0, "", "" } };
const int NIMPORTS = 0;
