#pragma once

#if defined(FRONTEND_CLIKE)
#define lookupType clike_lookupType
#define insertType clike_insertType
#define newASTNode clike_newASTNode
#define setTypeAST clike_setTypeAST
#define setRight clike_setRight
#define addChild clike_addChild
#define freeAST clike_freeAST
#define dumpAST clike_dumpAST
#define copyAST clike_copyAST
#define evaluateCompileTimeValue clike_evaluateCompileTimeValue
#endif

#if defined(FRONTEND_REA)
#define lookupType rea_lookupType
#define insertType rea_insertType
#define newASTNode rea_newASTNode
#define setTypeAST rea_setTypeAST
#define setRight rea_setRight
#define addChild rea_addChild
#define freeAST rea_freeAST
#define dumpAST rea_dumpAST
#define copyAST rea_copyAST
#define evaluateCompileTimeValue rea_evaluateCompileTimeValue
#endif

#if defined(FRONTEND_SHELL)
#define lookupType shell_lookupType
#define insertType shell_insertType
#define newASTNode shell_newASTNode
#define setTypeAST shell_setTypeAST
#define setRight shell_setRight
#define addChild shell_addChild
#define freeAST shell_freeAST
#define dumpAST shell_dumpAST
#define copyAST shell_copyAST
#define evaluateCompileTimeValue shell_evaluateCompileTimeValue
#endif
