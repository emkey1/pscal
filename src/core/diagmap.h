#ifndef PSCAL_CORE_DIAGMAP_H
#define PSCAL_CORE_DIAGMAP_H

/*
 * Rewritten-line -> original-source-line map: a shared, front-end-agnostic
 * diagnostic facility. Front ends that rewrite source before parsing (e.g.
 * Aether) record the mapping so the shared bytecode compiler and semantic
 * passes can report errors against the user's original source lines.
 *
 * Used by core (compiler/bytecode.c), Rea, and Aether. The function names
 * retain the historical aether* prefix; the facility itself is core.
 */
int aetherMapRewrittenLineToSource(int rewrittenLine);
int aetherHasRewriteLineMap(void);
int aetherNoteRewriteLineMapping(int rewrittenLine, int sourceLine);
void aetherClearRewriteLineMap(void);

#endif /* PSCAL_CORE_DIAGMAP_H */
