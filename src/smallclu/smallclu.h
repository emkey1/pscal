#ifndef SMALLCLU_SMALLCLU_H
#define SMALLCLU_SMALLCLU_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*SmallcluAppletEntry)(int argc, char **argv);

typedef struct SmallcluApplet {
    const char *name;
    SmallcluAppletEntry entry;
    const char *description;
} SmallcluApplet;

int smallcluMain(int argc, char **argv);

const SmallcluApplet *smallcluGetApplets(size_t *count);
const SmallcluApplet *smallcluFindApplet(const char *name);
int smallcluDispatchApplet(const SmallcluApplet *applet, int argc, char **argv);

void smallcluRegisterBuiltins(void);

#ifdef __cplusplus
}
#endif

#endif /* SMALLCLU_SMALLCLU_H */
