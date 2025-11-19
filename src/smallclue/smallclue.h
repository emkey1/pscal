#ifndef SMALLCLUE_SMALLCLUE_H
#define SMALLCLUE_SMALLCLUE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*SmallclueAppletEntry)(int argc, char **argv);

typedef struct SmallclueApplet {
    const char *name;
    SmallclueAppletEntry entry;
    const char *description;
} SmallclueApplet;

int smallclueMain(int argc, char **argv);

const SmallclueApplet *smallclueGetApplets(size_t *count);
const SmallclueApplet *smallclueFindApplet(const char *name);
int smallclueDispatchApplet(const SmallclueApplet *applet, int argc, char **argv);

void smallclueRegisterBuiltins(void);

#ifdef __cplusplus
}
#endif

#endif /* SMALLCLUE_SMALLCLUE_H */
