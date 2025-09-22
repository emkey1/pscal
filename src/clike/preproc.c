#include "clike/preproc.h"
#include "core/preproc.h"

char* clikePreprocess(const char *source, const char **defines, int define_count) {
    return preprocessConditionals(source, defines, define_count);
}
