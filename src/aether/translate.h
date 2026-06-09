#ifndef PSCAL_AETHER_TRANSLATE_H
#define PSCAL_AETHER_TRANSLATE_H

char *aetherRewriteSource(const char *source, const char *path);
int aetherMapRewrittenLineToSource(int rewrittenLine);
int aetherHasRewriteLineMap(void);
int aetherNoteRewriteLineMapping(int rewrittenLine, int sourceLine);
void aetherClearRewriteLineMap(void);

#endif
