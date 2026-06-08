#ifndef PSCAL_AETHER_TRANSLATE_H
#define PSCAL_AETHER_TRANSLATE_H

char *aetherRewriteSource(const char *source, const char *path);
int aetherMapRewrittenLineToSource(int rewrittenLine);
void aetherClearRewriteLineMap(void);

#endif
