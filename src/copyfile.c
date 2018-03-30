#define _COPYFILE_TEST
#define COPYFILE_DEBUG (1<<31)

#include <copyfile.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#include <R_ext/Rdynload.h>
#include <Rinternals.h>

SEXP copyfile_() {
  errno = 0;
  s = copyfile_state_alloc();
  int res = copyfile("a", "b", NULL, COPYFILE_ALL | COPYFILE_EXCL | COPYFILE_DEBUG);
  Rprintf("%i:%i:%s\n", res, errno, strerror(errno));
  return R_NilValue;
}

static const R_CallMethodDef CallEntries[] = {{"copyfile_", (DL_FUNC)&copyfile_, 0},
                                              {NULL, NULL, 0}};

void R_init_copyfile(DllInfo *dll) {
  R_registerRoutines(dll, NULL, CallEntries, NULL, NULL);
  R_useDynamicSymbols(dll, FALSE);
}
