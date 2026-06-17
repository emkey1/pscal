/*
 * MIT License
 *
 * Copyright (c) 2024 PSCAL contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Note: PSCAL versions prior to 2.22 were released under the Unlicense.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <ctype.h>
#include <unistd.h>
#include <signal.h>
#include "vm/vm.h"
#include "core/cache.h"
#include "core/utils.h"
#include "core/preproc.h"
#include "core/build_info.h"
#include "symbol/symbol.h"
#include <fcntl.h>
#include <errno.h>
#include "Pascal/globals.h"
#include "ast/ast.h"
#include "compiler/bytecode.h"
#include "compiler/compiler.h"
#include "backend_ast/builtin.h"
#include "common/frontend_kind.h"
#include "rea/builtins/thread.h"
#include "rea/parser.h"
#include "rea/state.h"
#include "rea/semantic.h"
#include "aether/parser.h"
#ifdef PSCAL_FRONTEND_HAS_REWRITE_DUMP
#include "aether/translate.h"
#endif
#include "aether/state.h"
#include "aether/semantic.h"
#include "ext_builtins/dump.h"
#include "common/path_virtualization.h"

#ifndef PSCAL_FRONTEND_MAIN_NAME
#define PSCAL_FRONTEND_MAIN_NAME rea_main
#endif

#ifndef PSCAL_FRONTEND_KIND
#define PSCAL_FRONTEND_KIND FRONTEND_KIND_REA
#endif

#ifndef PSCAL_FRONTEND_DISPLAY_NAME
#define PSCAL_FRONTEND_DISPLAY_NAME "Rea"
#endif

#ifndef PSCAL_FRONTEND_USAGE_NAME
#define PSCAL_FRONTEND_USAGE_NAME "rea"
#endif

#ifndef PSCAL_FRONTEND_COMPILER_ID
#define PSCAL_FRONTEND_COMPILER_ID "rea"
#endif

#ifndef PSCAL_FRONTEND_PARSE_SOURCE
#define PSCAL_FRONTEND_PARSE_SOURCE(source) parseRea(source)
#endif

#ifndef PSCAL_FRONTEND_SET_STRICT_MODE
#define PSCAL_FRONTEND_SET_STRICT_MODE(enable) reaSetStrictMode(enable)
#endif

#ifndef PSCAL_FRONTEND_RESET_SYMBOL_STATE
#define PSCAL_FRONTEND_RESET_SYMBOL_STATE() reaResetSymbolState()
#endif

#ifndef PSCAL_FRONTEND_INVALIDATE_GLOBAL_STATE
#define PSCAL_FRONTEND_INVALIDATE_GLOBAL_STATE() reaInvalidateGlobalState()
#endif

#ifndef PSCAL_FRONTEND_SEMANTIC_SET_SOURCE_PATH
#define PSCAL_FRONTEND_SEMANTIC_SET_SOURCE_PATH(path) reaSemanticSetSourcePath(path)
#endif

#ifndef PSCAL_FRONTEND_PERFORM_SEMANTIC_ANALYSIS
#define PSCAL_FRONTEND_PERFORM_SEMANTIC_ANALYSIS(root) reaPerformSemanticAnalysis(root)
#endif

#ifndef PSCAL_FRONTEND_GET_LOADED_MODULE_COUNT
#define PSCAL_FRONTEND_GET_LOADED_MODULE_COUNT() reaGetLoadedModuleCount()
#endif

#ifndef PSCAL_FRONTEND_GET_MODULE_AST
#define PSCAL_FRONTEND_GET_MODULE_AST(index) reaGetModuleAST(index)
#endif

#ifndef PSCAL_FRONTEND_GET_MODULE_PATH
#define PSCAL_FRONTEND_GET_MODULE_PATH(index) reaGetModulePath(index)
#endif

#ifndef PSCAL_FRONTEND_GET_MODULE_NAME
#define PSCAL_FRONTEND_GET_MODULE_NAME(index) reaGetModuleName(index)
#endif

#ifndef PSCAL_FRONTEND_RESOLVE_IMPORT_PATH
#define PSCAL_FRONTEND_RESOLVE_IMPORT_PATH(path) reaResolveImportPath(path)
#endif

static void initSymbolSystem(void) {
    globalSymbols = createHashTable();
    constGlobalSymbols = createHashTable();
    procedure_table = createHashTable();
    insertStandardStreamSymbols();
    current_procedure_table = procedure_table;
}

static VM *g_sigint_vm = NULL;

static void reaHandleSigint(int signo) {
    (void)signo;
    if (g_sigint_vm) {
        g_sigint_vm->abort_requested = true;
        g_sigint_vm->exit_requested = true;
    }
}

static void reaInstallSigint(void) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigprocmask(SIG_UNBLOCK, &set, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = reaHandleSigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
}

static const char *REA_USAGE =
    "Usage: " PSCAL_FRONTEND_USAGE_NAME " <options> <source> [program_parameters...]\n"
    "   Options:\n"
    "     -v                     Display version.\n"
    "     --dump-ast-json        Dump AST to JSON and exit.\n"
#ifdef PSCAL_FRONTEND_HAS_REWRITE_DUMP
    "     --dump-rewrite         Dump frontend-rewritten source and exit.\n"
#endif
    "     --dump-bytecode        Dump compiled bytecode before execution.\n"
    "     --dump-bytecode-only   Dump compiled bytecode and exit (no execution).\n"
    "     --no-run               Compile but skip VM execution.\n"
    "     --dump-ext-builtins    List extended builtin inventory and exit.\n"
    "     --no-cache             Compile fresh (ignore cached bytecode).\n"
    "     --verbose              Print compilation/cache status messages.\n"
#if PSCAL_FRONTEND_KIND == FRONTEND_KIND_AETHER
    "     --verbose-compat       Print Aether compatibility warnings such as ignored missing imports.\n"
#endif
    "     --strict               Enable strict parser checks for top-level structure.\n"
    "     --diagnostics-json     Emit compiler diagnostics as JSON on failure.\n"
    "     --diagnostics-toon     Emit compiler diagnostics as TOON on failure.\n"
    "     --vm-trace-head=N      Trace first N instructions in the VM (also enabled by '{trace on}' in source).\n"
    "\n"
    "   Thread helpers available to JSON snippets and the REPL:\n"
    "     thread_spawn_named(target, name, ...)  Launch allow-listed builtin on worker thread.\n"
    "     thread_pool_submit(target, name, ...) Queue work on the shared pool for asynchronous execution.\n"
    "     thread_pause/resume/cancel(handle)    Control pooled workers (returns 1 on success).\n"
    "     thread_get_status(handle, drop)       Inspect success flags (drop non-zero releases the slot).\n"
    "     thread_stats()                        Array of records summarizing pool usage.\n";

static const char *const kReaCompilerId = PSCAL_FRONTEND_COMPILER_ID;

typedef struct DiagnosticCapture {
    int enabled;
    int saved_stderr_fd;
    FILE *tmp;
} DiagnosticCapture;

typedef struct ParsedDiagnostic {
    char *file;
    int line;
    char *phase;
    char *kind;
    char *code;
    char *message;
    char *hint;
    char *raw;
} ParsedDiagnostic;

typedef struct RuntimeLocationInfo {
    size_t offset;
    int line;
} RuntimeLocationInfo;

static void freeParsedDiagnostics(ParsedDiagnostic *items, int count) {
    if (!items) return;
    for (int i = 0; i < count; i++) {
        free(items[i].file);
        free(items[i].phase);
        free(items[i].kind);
        free(items[i].code);
        free(items[i].message);
        free(items[i].hint);
        free(items[i].raw);
    }
    free(items);
}

static char *diagDupRange(const char *start, const char *end) {
    if (!start || !end || end < start) return NULL;
    size_t len = (size_t)(end - start);
    char *copy = (char *)malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, start, len);
    copy[len] = '\0';
    return copy;
}

static const char *trimEnd(const char *start, const char *end) {
    while (end > start && (end[-1] == '\n' || end[-1] == '\r' ||
                           end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    return end;
}

static int diagnosticCaptureBegin(DiagnosticCapture *capture) {
    if (!capture || !capture->enabled) return 1;
    capture->saved_stderr_fd = dup(STDERR_FILENO);
    if (capture->saved_stderr_fd < 0) {
        return 0;
    }
    capture->tmp = tmpfile();
    if (!capture->tmp) {
        close(capture->saved_stderr_fd);
        capture->saved_stderr_fd = -1;
        return 0;
    }
    fflush(stderr);
    if (dup2(fileno(capture->tmp), STDERR_FILENO) < 0) {
        fclose(capture->tmp);
        capture->tmp = NULL;
        close(capture->saved_stderr_fd);
        capture->saved_stderr_fd = -1;
        return 0;
    }
    return 1;
}

static void diagnosticCaptureEnd(DiagnosticCapture *capture) {
    if (!capture || !capture->enabled) return;
    fflush(stderr);
    if (capture->saved_stderr_fd >= 0) {
        dup2(capture->saved_stderr_fd, STDERR_FILENO);
        close(capture->saved_stderr_fd);
        capture->saved_stderr_fd = -1;
    }
}

static char *diagnosticCaptureRead(DiagnosticCapture *capture) {
    long size;
    char *text;

    if (!capture || !capture->tmp) return NULL;
    fflush(capture->tmp);
    if (fseek(capture->tmp, 0, SEEK_END) != 0) return NULL;
    size = ftell(capture->tmp);
    if (size < 0) return NULL;
    if (fseek(capture->tmp, 0, SEEK_SET) != 0) return NULL;
    text = (char *)malloc((size_t)size + 1);
    if (!text) return NULL;
    if (size > 0 && fread(text, 1, (size_t)size, capture->tmp) != (size_t)size) {
        free(text);
        return NULL;
    }
    text[size] = '\0';
    return text;
}

static void diagnosticCaptureDispose(DiagnosticCapture *capture) {
    if (!capture) return;
    diagnosticCaptureEnd(capture);
    if (capture->tmp) {
        fclose(capture->tmp);
        capture->tmp = NULL;
    }
}

static void jsonWriteEscaped(FILE *out, const char *text) {
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    fputc('"', out);
    while (*p) {
        switch (*p) {
            case '\\': fputs("\\\\", out); break;
            case '"': fputs("\\\"", out); break;
            case '\n': fputs("\\n", out); break;
            case '\r': fputs("\\r", out); break;
            case '\t': fputs("\\t", out); break;
            default:
                if (*p < 0x20) {
                    fprintf(out, "\\u%04x", *p);
                } else {
                    fputc(*p, out);
                }
                break;
        }
        p++;
    }
    fputc('"', out);
}

static void toonWriteEscaped(FILE *out, const char *text) {
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    while (*p) {
        switch (*p) {
            case '\\': fputs("\\\\", out); break;
            case '"': fputs("\\\"", out); break;
            case '\n': fputs("\\n", out); break;
            case '\r': fputs("\\r", out); break;
            case '\t': fputs("\\t", out); break;
            default:
                fputc(*p, out);
                break;
        }
        p++;
    }
}

static void inferDiagnosticPhaseKind(const char *message, char **outPhase, char **outKind) {
    const char *phase = "compile";
    const char *kind = "generic";

    if (message) {
        if (strstr(message, "rewrite error")) {
            phase = "rewrite";
            if (strstr(message, "declaration")) kind = "declaration";
            else if (strstr(message, "TOON")) kind = "toon";
            else if (strstr(message, "par")) kind = "par";
            else kind = "rewrite";
        } else if (strstr(message, "Aether contract error")) {
            phase = "semantic";
            kind = "contract";
        } else if (strstr(message, "Aether effect error")) {
            phase = "semantic";
            kind = "effect";
        } else if (strstr(message, "Aether purity error")) {
            phase = "semantic";
            kind = "purity";
        } else if (strstr(message, "Aether import error")) {
            phase = "semantic";
            kind = "import";
        } else if (strstr(message, "Aether type error")) {
            phase = "semantic";
            kind = "type";
        } else if (strstr(message, "not in scope")) {
            phase = "semantic";
            kind = "scope";
        } else if (strstr(message, "not callable")) {
            phase = "semantic";
            kind = "call";
        } else if (strstr(message, "Not enough arguments") ||
                   strstr(message, "Too many arguments")) {
            phase = "semantic";
            kind = "arity";
        } else if (strstr(message, "Compilation failed")) {
            phase = "compile";
            kind = "compiler";
        }
    }

    *outPhase = diagDupRange(phase, phase + strlen(phase));
    *outKind = diagDupRange(kind, kind + strlen(kind));
}

static void extractDiagnosticCode(ParsedDiagnostic *diag) {
    const char *message;
    const char *close;
    const char *after;
    char *strippedMessage;

    if (!diag || !diag->message || diag->message[0] != '[') {
        return;
    }

    message = diag->message;
    close = strchr(message, ']');
    if (!close || close == message + 1) {
        return;
    }

    after = close + 1;
    if (*after == ' ') {
        after++;
    }

    diag->code = diagDupRange(message + 1, close);
    if (!diag->code) {
        free(diag->code);
        diag->code = NULL;
        return;
    }

    strippedMessage = diagDupRange(after, after + strlen(after));
    if (!strippedMessage) {
        free(diag->code);
        diag->code = NULL;
        return;
    }
    free(diag->message);
    diag->message = strippedMessage;
}

static ParsedDiagnostic parseDiagnosticLine(const char *line) {
    ParsedDiagnostic diag = {0};
    const char *firstColon;
    const char *secondColon;
    char *endptr = NULL;

    if (!line) return diag;
    diag.raw = diagDupRange(line, line + strlen(line));

    firstColon = strchr(line, ':');
    if (firstColon && firstColon[1] && isdigit((unsigned char)firstColon[1])) {
        long parsedLine;
        secondColon = strchr(firstColon + 1, ':');
        if (secondColon) {
            parsedLine = strtol(firstColon + 1, &endptr, 10);
            if (endptr == secondColon) {
                const char *message = secondColon + 1;
                while (*message == ' ') message++;
                diag.file = diagDupRange(line, firstColon);
                diag.line = (int)parsedLine;
                diag.message = diagDupRange(message, message + strlen(message));
                extractDiagnosticCode(&diag);
                inferDiagnosticPhaseKind(diag.message, &diag.phase, &diag.kind);
                return diag;
            }
        }
    }

    if (line[0] == 'L' && isdigit((unsigned char)line[1])) {
        long parsedLine = strtol(line + 1, &endptr, 10);
        if (endptr && endptr[0] == ':' && endptr[1] == ' ') {
            const char *message = endptr + 2;
            diag.line = (int)parsedLine;
            diag.message = diagDupRange(message, message + strlen(message));
            extractDiagnosticCode(&diag);
            inferDiagnosticPhaseKind(diag.message, &diag.phase, &diag.kind);
            return diag;
        }
    }

    diag.message = diagDupRange(line, line + strlen(line));
    extractDiagnosticCode(&diag);
    inferDiagnosticPhaseKind(diag.message, &diag.phase, &diag.kind);
    return diag;
}

static int parseRuntimeLocationLine(const char *line, RuntimeLocationInfo *info) {
    unsigned long long parsedOffset = 0;
    int parsedLine = 0;

    if (!line || !info) return 0;
    if (sscanf(line, "[Error Location] Offset: %llu, Line: %d", &parsedOffset, &parsedLine) == 2) {
        info->offset = (size_t)parsedOffset;
        info->line = parsedLine;
        return 1;
    }
    if (sscanf(line, "[Warning Location] Offset: %llu, Line: %d", &parsedOffset, &parsedLine) == 2) {
        info->offset = (size_t)parsedOffset;
        info->line = parsedLine;
        return 1;
    }
    return 0;
}

static ParsedDiagnostic *collectDiagnosticsFromText(const char *text, int *outCount) {
    ParsedDiagnostic *items = NULL;
    int count = 0;
    int capacity = 0;
    const char *cursor = text ? text : "";

    while (*cursor) {
        const char *lineStart = cursor;
        const char *lineEnd = cursor;
        const char *rawLineEnd;
        ParsedDiagnostic diag;

        while (*lineEnd && *lineEnd != '\n') lineEnd++;
        rawLineEnd = lineEnd;
        lineEnd = trimEnd(lineStart, lineEnd);
        if (lineEnd > lineStart) {
            char *line = diagDupRange(lineStart, lineEnd);
            if (line && strncmp(line, "hint: ", 6) == 0 && count > 0) {
                free(items[count - 1].hint);
                items[count - 1].hint = diagDupRange(line + 6, line + strlen(line));
                free(line);
            } else if (line) {
                if (count == capacity) {
                    int newCap = capacity ? capacity * 2 : 8;
                    ParsedDiagnostic *resized =
                        (ParsedDiagnostic *)realloc(items, (size_t)newCap * sizeof(ParsedDiagnostic));
                    if (!resized) {
                        free(line);
                        freeParsedDiagnostics(items, count);
                        return NULL;
                    }
                    items = resized;
                    capacity = newCap;
                }
                diag = parseDiagnosticLine(line);
                items[count++] = diag;
                free(line);
            }
        }
        cursor = *rawLineEnd == '\0' ? rawLineEnd : rawLineEnd + 1;
    }

    if (outCount) *outCount = count;
    return items;
}

static ParsedDiagnostic *collectRuntimeDiagnosticsFromText(const char *text,
                                                          const char *defaultFile,
                                                          int *outCount) {
    static const char kRuntimeText[] = "runtime";
    ParsedDiagnostic *items = NULL;
    int count = 0;
    int capacity = 0;
    const char *cursor = text ? text : "";

    while (*cursor) {
        const char *lineStart = cursor;
        const char *lineEnd = cursor;
        const char *rawLineEnd;
        RuntimeLocationInfo location = {0};

        while (*lineEnd && *lineEnd != '\n') lineEnd++;
        rawLineEnd = lineEnd;
        lineEnd = trimEnd(lineStart, lineEnd);
        if (lineEnd > lineStart) {
            char *line = diagDupRange(lineStart, lineEnd);
            if (!line) {
                freeParsedDiagnostics(items, count);
                return NULL;
            }
            if (parseRuntimeLocationLine(line, &location)) {
                if (count > 0) {
                    items[count - 1].line = location.line;
                }
                free(line);
            } else {
                ParsedDiagnostic diag;
                if (count == capacity) {
                    int newCap = capacity ? capacity * 2 : 4;
                    ParsedDiagnostic *resized =
                        (ParsedDiagnostic *)realloc(items, (size_t)newCap * sizeof(ParsedDiagnostic));
                    if (!resized) {
                        free(line);
                        freeParsedDiagnostics(items, count);
                        return NULL;
                    }
                    items = resized;
                    capacity = newCap;
                }
                diag = parseDiagnosticLine(line);
                free(diag.file);
                diag.file = defaultFile ? diagDupRange(defaultFile, defaultFile + strlen(defaultFile)) : NULL;
                free(diag.phase);
                diag.phase = diagDupRange(kRuntimeText, kRuntimeText + 7);
                free(diag.kind);
                diag.kind = diagDupRange(kRuntimeText, kRuntimeText + 7);
                items[count++] = diag;
                free(line);
            }
        }
        cursor = *rawLineEnd == '\0' ? rawLineEnd : rawLineEnd + 1;
    }

    if (outCount) *outCount = count;
    return items;
}

static void emitDiagnosticsJson(FILE *out, ParsedDiagnostic *items, int count) {
    fputs("[\n", out);
    for (int i = 0; i < count; i++) {
        ParsedDiagnostic *diag = &items[i];
        fputs("  {\"severity\":\"error\"", out);
        fputs(",\"phase\":", out);
        jsonWriteEscaped(out, diag->phase ? diag->phase : "compile");
        fputs(",\"kind\":", out);
        jsonWriteEscaped(out, diag->kind ? diag->kind : "generic");
        fputs(",\"code\":", out);
        if (diag->code) {
            jsonWriteEscaped(out, diag->code);
        } else {
            fputs("null", out);
        }
        fputs(",\"file\":", out);
        if (diag->file) {
            jsonWriteEscaped(out, diag->file);
        } else {
            fputs("null", out);
        }
        fprintf(out, ",\"line\":%d", diag->line);
        fputs(",\"column\":null", out);
        fputs(",\"message\":", out);
        jsonWriteEscaped(out, diag->message ? diag->message : "");
        fputs(",\"hint\":", out);
        if (diag->hint) {
            jsonWriteEscaped(out, diag->hint);
        } else {
            fputs("null", out);
        }
        fputs(",\"raw\":", out);
        jsonWriteEscaped(out, diag->raw ? diag->raw : "");
        fputs("}", out);
        if (i + 1 < count) fputs(",", out);
        fputs("\n", out);
    }
    fputs("]\n", out);
}

static void emitDiagnosticsToon(FILE *out, ParsedDiagnostic *items, int count) {
    fprintf(out, "diagnostics[%d]{severity,phase,kind,code,file,line,column,message,hint,raw}:\n", count);
    for (int i = 0; i < count; i++) {
        ParsedDiagnostic *diag = &items[i];
        fputs("  \"", out);
        toonWriteEscaped(out, "error");
        fputs("\",\"", out);
        toonWriteEscaped(out, diag->phase ? diag->phase : "compile");
        fputs("\",\"", out);
        toonWriteEscaped(out, diag->kind ? diag->kind : "generic");
        fputs("\",\"", out);
        toonWriteEscaped(out, diag->code ? diag->code : "");
        fputs("\",\"", out);
        toonWriteEscaped(out, diag->file ? diag->file : "");
        fprintf(out, "\",%d,null,\"", diag->line);
        toonWriteEscaped(out, diag->message ? diag->message : "");
        fputs("\",\"", out);
        toonWriteEscaped(out, diag->hint ? diag->hint : "");
        fputs("\",\"", out);
        toonWriteEscaped(out, diag->raw ? diag->raw : "");
        fputs("\"\n", out);
    }
}

static void emitDiagnosticsFromText(FILE *out, const char *text, int asToon) {
    int count = 0;
    ParsedDiagnostic *items = collectDiagnosticsFromText(text, &count);

    if (!items && count == 0) {
        if (asToon) {
            fputs("diagnostics[0]{severity,phase,kind,code,file,line,column,message,hint,raw}:\n", out);
        } else {
            fputs("[]\n", out);
        }
        return;
    }

    if (asToon) {
        emitDiagnosticsToon(out, items, count);
    } else {
        emitDiagnosticsJson(out, items, count);
    }

    freeParsedDiagnostics(items, count);
}

static void emitRuntimeDiagnosticsFromText(FILE *out,
                                           const char *text,
                                           const char *defaultFile,
                                           int asToon) {
    int count = 0;
    ParsedDiagnostic *items = collectRuntimeDiagnosticsFromText(text, defaultFile, &count);

    if (!items && count == 0) {
        if (asToon) {
            fputs("diagnostics[0]{severity,phase,kind,code,file,line,column,message,hint,raw}:\n", out);
        } else {
            fputs("[]\n", out);
        }
        return;
    }

    if (asToon) {
        emitDiagnosticsToon(out, items, count);
    } else {
        emitDiagnosticsJson(out, items, count);
    }

    freeParsedDiagnostics(items, count);
}

static bool isUnitListFresh(List* unit_list, time_t cache_mtime) {
    if (!unit_list) return true;
#if defined(__APPLE__)
#define PSCAL_STAT_SEC(st)  ((st).st_mtimespec.tv_sec)
#else
#define PSCAL_STAT_SEC(st)  ((st).st_mtim.tv_sec)
#endif
    for (int i = 0; i < listSize(unit_list); i++) {
        char *used_unit_name = (char*)listGet(unit_list, i);
        if (!used_unit_name) continue;

        char lower_used_unit_name[MAX_SYMBOL_LENGTH];
        strncpy(lower_used_unit_name, used_unit_name, MAX_SYMBOL_LENGTH - 1);
        lower_used_unit_name[MAX_SYMBOL_LENGTH - 1] = '\0';
        for (int k = 0; lower_used_unit_name[k]; k++) {
            lower_used_unit_name[k] = tolower((unsigned char)lower_used_unit_name[k]);
        }

        char *unit_file_path = findUnitFile(lower_used_unit_name);
        if (!unit_file_path) continue;

        struct stat unit_stat;
        if (stat(unit_file_path, &unit_stat) != 0) {
            free(unit_file_path);
            return false;
        }
        if (cache_mtime <= PSCAL_STAT_SEC(unit_stat)) {
            free(unit_file_path);
            return false;
        }
        free(unit_file_path);
    }
#undef PSCAL_STAT_SEC
    return true;
}

static bool importsAreFresh(AST* node, time_t cache_mtime) {
    if (!node) return true;
    if (node->type == AST_USES_CLAUSE && node->unit_list) {
        if (!isUnitListFresh(node->unit_list, cache_mtime)) return false;
    }
    if (node->left && !importsAreFresh(node->left, cache_mtime)) return false;
    if (node->right && !importsAreFresh(node->right, cache_mtime)) return false;
    if (node->extra && !importsAreFresh(node->extra, cache_mtime)) return false;
    for (int i = 0; i < node->child_count; i++) {
        if (node->children[i] && !importsAreFresh(node->children[i], cache_mtime)) return false;
    }
    return true;
}

/*
 * Rea has its own native module system (module / #import, compiled via
 * compileModuleAST). The legacy Pascal-unit path that borrowed Pascal's
 * unitParser was unreachable here (Rea never populates AST_USES_CLAUSE.unit_list)
 * and has been removed, so Rea no longer depends on the Pascal front end.
 */
static void collectUsesClauses(AST* node, List* out) {
    if (!node) return;
    if (node->type == AST_IMPORT && node->token && node->token->value) {
        char *resolved = PSCAL_FRONTEND_RESOLVE_IMPORT_PATH(node->token->value);
        if (resolved) {
            listAppend(out, resolved);
            free(resolved);
        } else {
            listAppend(out, node->token->value);
        }
    }
    if (node->left) collectUsesClauses(node->left, out);
    if (node->right) collectUsesClauses(node->right, out);
    if (node->extra) collectUsesClauses(node->extra, out);
    for (int i = 0; i < node->child_count; i++) {
        if (node->children[i]) collectUsesClauses(node->children[i], out);
    }
}

int PSCAL_FRONTEND_MAIN_NAME(int argc, char **argv) {
    /* Always start from a clean slate in case a prior in-process run aborted
     * early (e.g., exit()/halt during startup). */
    PSCAL_FRONTEND_INVALIDATE_GLOBAL_STATE();

    /* Skip process-wide fd redirection on iOS; background jobs share descriptors with the shell. */
#if !defined(PSCAL_TARGET_IOS)
    const char *stdout_path = getenv("PSCALI_BG_STDOUT");
    const char *stdout_append = getenv("PSCALI_BG_STDOUT_APPEND");
    const char *stderr_path = getenv("PSCALI_BG_STDERR");
    const char *stderr_append = getenv("PSCALI_BG_STDERR_APPEND");
    if (stdout_path && *stdout_path) {
        int flags = O_CREAT | O_WRONLY | ((stdout_append && strcmp(stdout_append, "1") == 0) ? O_APPEND : O_TRUNC);
        int fd = open(stdout_path, flags, 0666);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            close(fd);
        }
    }
    if (stderr_path && *stderr_path) {
        int flags = O_CREAT | O_WRONLY | ((stderr_append && strcmp(stderr_append, "1") == 0) ? O_APPEND : O_TRUNC);
        int fd = open(stderr_path, flags, 0666);
        if (fd >= 0) {
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
    } else if (stdout_path && *stdout_path && stderr_append && strcmp(stderr_append, "1") == 0) {
        dup2(STDOUT_FILENO, STDERR_FILENO);
    }
#endif

    FrontendKind previousKind = frontendPushKind(PSCAL_FRONTEND_KIND);
#define REA_RETURN(value)                           \
    do {                                            \
        int __rea_rc = (value);                     \
        if (reaSymbolStateActive) {                 \
            PSCAL_FRONTEND_RESET_SYMBOL_STATE();    \
            reaSymbolStateActive = false;           \
        }                                           \
        frontendPopKind(previousKind);              \
        return __rea_rc;                            \
    } while (0)

    const char *initTerm = getenv("PSCAL_INIT_TERM");
    if (initTerm && *initTerm && *initTerm != '0') {
        vmInitTerminalState();
    }

    int dump_ast_json = 0;
#ifdef PSCAL_FRONTEND_HAS_REWRITE_DUMP
    int dump_rewrite = 0;
#endif
    int dump_bytecode_flag = 0;
    int dump_bytecode_only_flag = 0;
    int no_run_flag = 0;
    int dump_ext_builtins = 0;
    int vm_trace_head = 0;
    int no_cache = 0;
#ifdef PSCAL_TARGET_IOS
    /* Cached bytecode compiled by a different app binary can drift out of sync
     * on iOS because tools run in-process. Default to fresh compiles unless the
     * user explicitly opts back in via REA_CACHE=1. */
    const char *rea_cache_env = getenv("REA_CACHE");
    if (!rea_cache_env || rea_cache_env[0] == '\0' || rea_cache_env[0] == '0') {
        no_cache = 1;
    }
#endif
    int verbose_flag = 0;
    int strict_mode = 0;
    int verbose_compat = 0;
    int diagnostics_json = 0;
    int diagnostics_toon = 0;
    int argi = 1;
    bool reaSymbolStateActive = false;
    /* Clear any stale compiler/unit state that might linger when invoked
     * repeatedly from the shell tool runner. */
    compilerResetState();
    if (!argv || argc <= 0) {
        REA_RETURN(vmExitWithCleanup(EXIT_FAILURE));
    }
    while (argc > argi && argv && argv[argi] && argv[argi][0] == '-') {
        if (strcmp(argv[argi], "-h") == 0 || strcmp(argv[argi], "--help") == 0) {
            printf("%s", REA_USAGE);
            REA_RETURN(vmExitWithCleanup(EXIT_SUCCESS));
        } else if (strcmp(argv[argi], "-v") == 0 || strcmp(argv[argi], "--version") == 0) {
            printf(PSCAL_FRONTEND_DISPLAY_NAME " Compiler Version: %s (latest tag: %s)\n",
                   pscal_program_version_string(), pscal_git_tag_string());
            REA_RETURN(vmExitWithCleanup(EXIT_SUCCESS));
        } else if (strcmp(argv[argi], "--dump-ast-json") == 0) {
            dump_ast_json = 1;
#ifdef PSCAL_FRONTEND_HAS_REWRITE_DUMP
        } else if (strcmp(argv[argi], "--dump-rewrite") == 0) {
            dump_rewrite = 1;
#endif
        } else if (strcmp(argv[argi], "--dump-bytecode") == 0) {
            dump_bytecode_flag = 1;
        } else if (strcmp(argv[argi], "--dump-bytecode-only") == 0) {
            dump_bytecode_flag = 1;
            dump_bytecode_only_flag = 1;
        } else if (strcmp(argv[argi], "--no-run") == 0) {
            no_run_flag = 1;
        } else if (strcmp(argv[argi], "--dump-ext-builtins") == 0) {
            dump_ext_builtins = 1;
        } else if (strcmp(argv[argi], "--no-cache") == 0) {
            no_cache = 1;
        } else if (strcmp(argv[argi], "--verbose") == 0) {
            verbose_flag = 1;
        } else if (strcmp(argv[argi], "--verbose-compat") == 0 ||
                   strcmp(argv[argi], "--verbose-errors") == 0) {
            verbose_compat = 1;
        } else if (strcmp(argv[argi], "--strict") == 0) {
            strict_mode = 1;
        } else if (strcmp(argv[argi], "--diagnostics-json") == 0) {
            diagnostics_json = 1;
        } else if (strcmp(argv[argi], "--diagnostics-toon") == 0) {
            diagnostics_toon = 1;
        } else if (strncmp(argv[argi], "--vm-trace-head=", 16) == 0) {
            vm_trace_head = atoi(argv[argi] + 16);
        } else {
            fprintf(stderr, "Unknown option: %s\n%s", argv[argi], REA_USAGE);
            REA_RETURN(vmExitWithCleanup(EXIT_FAILURE));
        }
        argi++;
    }

    if (dump_ext_builtins) {
        registerExtendedBuiltins();
        extBuiltinDumpInventory(stdout);
        REA_RETURN(vmExitWithCleanup(EXIT_SUCCESS));
    }

    if (argc <= argi) {
        fprintf(stderr, "%s", REA_USAGE);
        REA_RETURN(vmExitWithCleanup(EXIT_FAILURE));
    }

    const char *path = argv[argi++];
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror("open");
        REA_RETURN(vmExitWithCleanup(EXIT_FAILURE));
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    char *src = (char *)malloc(len + 1);
    if (!src) {
        fclose(f);
        REA_RETURN(vmExitWithCleanup(EXIT_FAILURE));
    }
    size_t bytes_read = fread(src, 1, len, f);
    fclose(f);
    if (bytes_read != (size_t)len) {
        free(src);
        fprintf(stderr, "Error reading source file '%s'\n", path);
        REA_RETURN(vmExitWithCleanup(EXIT_FAILURE));
    }
    src[len] = '\0';

    const char *defines[1] = {NULL};
    int define_count = 0;
#ifdef SDL
    defines[define_count++] = "SDL_ENABLED";
#endif
    char *preprocessed_source = preprocessConditionals(src, defines, define_count);
    const char *effective_src = preprocessed_source ? preprocessed_source : src;

    aetherSetVerboseCompatibilityDiagnostics(
        frontendGetKind() == FRONTEND_KIND_AETHER ? verbose_compat : 0);

#ifdef PSCAL_FRONTEND_HAS_REWRITE_DUMP
    if (dump_rewrite) {
        char *rewritten = aetherRewriteSource(effective_src, path);
        if (!rewritten) {
            if (preprocessed_source) free(preprocessed_source);
            free(src);
            REA_RETURN(vmExitWithCleanup(EXIT_FAILURE));
        }
        fputs(rewritten, stdout);
        if (rewritten[0] != '\0' && rewritten[strlen(rewritten) - 1] != '\n') {
            fputc('\n', stdout);
        }
        free(rewritten);
        if (preprocessed_source) free(preprocessed_source);
        free(src);
        REA_RETURN(vmExitWithCleanup(EXIT_SUCCESS));
    }
#endif
    // Note: Bootstrap of entrypoint is disabled; rely on source top-level or
    // future bytecode-level CALL injection.

    initSymbolSystem();
    reaSymbolStateActive = true;
    /* Rea uses friendly auto-spacing between write() arguments. Aether does NOT:
     * a write/writeln must emit its arguments verbatim so the programmer controls
     * spacing exactly (no "magic" spaces that can't be removed). Aether lowers to
     * Rea, so suppress the spacing specifically for the Aether frontend. */
    gSuppressWriteSpacing = (frontendGetKind() == FRONTEND_KIND_AETHER) ? 1 : 0;
    gUppercaseBooleans = 0;
    registerAllBuiltins();
    reaRegisterThreadBuiltins();
    /* memory stream helpers */
    registerBuiltinFunction("mstreamappendbyte", AST_FUNCTION_DECL, NULL);
    /* C-like style cast helpers */
    registerBuiltinFunction("int", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("double", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("float", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("char", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("bool", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("byte", AST_FUNCTION_DECL, NULL);
    /* synonyms to avoid keyword collisions */
    registerBuiltinFunction("toint", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("todouble", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("tofloat", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("tochar", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("tobool", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("tobyte", AST_FUNCTION_DECL, NULL);

    DiagnosticCapture diagCapture = {
        .enabled = diagnostics_json,
        .saved_stderr_fd = -1,
        .tmp = NULL
    };
    if (diagnostics_toon) {
        diagCapture.enabled = 1;
    }
    if (!diagnosticCaptureBegin(&diagCapture)) {
        fprintf(stderr, "Failed to initialize diagnostics capture: %s\n", strerror(errno));
        if (preprocessed_source) free(preprocessed_source);
        free(src);
        REA_RETURN(vmExitWithCleanup(EXIT_FAILURE));
    }

    if (strict_mode) PSCAL_FRONTEND_SET_STRICT_MODE(1);
    PSCAL_FRONTEND_SEMANTIC_SET_SOURCE_PATH(path);
    AST *program = PSCAL_FRONTEND_PARSE_SOURCE(effective_src);
    if (!program) {
        diagnosticCaptureEnd(&diagCapture);
        if (diagnostics_json || diagnostics_toon) {
            char *diagText = diagnosticCaptureRead(&diagCapture);
            emitDiagnosticsFromText(stderr, diagText, diagnostics_toon);
            free(diagText);
        }
        diagnosticCaptureDispose(&diagCapture);
        if (preprocessed_source) free(preprocessed_source);
        free(src);
        REA_RETURN(vmExitWithCleanup(EXIT_FAILURE));
    }
    PSCAL_FRONTEND_PERFORM_SEMANTIC_ANALYSIS(program);
    if (pascal_semantic_error_count > 0 && !dump_ast_json) {
        diagnosticCaptureEnd(&diagCapture);
        if (diagnostics_json || diagnostics_toon) {
            char *diagText = diagnosticCaptureRead(&diagCapture);
            emitDiagnosticsFromText(stderr, diagText, diagnostics_toon);
            free(diagText);
        }
        diagnosticCaptureDispose(&diagCapture);
        freeAST(program);
        if (preprocessed_source) free(preprocessed_source);
        free(src);
        REA_RETURN(vmExitWithCleanup(EXIT_FAILURE));
    }
    if (dump_ast_json) {
        diagnosticCaptureEnd(&diagCapture);
        diagnosticCaptureDispose(&diagCapture);
        annotateTypes(program, NULL, program);
        dumpASTJSON(program, stdout);
        freeAST(program);
        if (preprocessed_source) free(preprocessed_source);
        free(src);
        REA_RETURN(vmExitWithCleanup(EXIT_SUCCESS));
    }

    List *dep_files = createList();
    collectUsesClauses(program, dep_files);
    int dep_count = listSize(dep_files);
    const char **dep_array = NULL;
    if (dep_count > 0) {
        dep_array = malloc(sizeof(char*) * dep_count);
        for (int i = 0; i < dep_count; i++) dep_array[i] = listGet(dep_files, i);
    }

    BytecodeChunk chunk;
    initBytecodeChunk(&chunk);
    setBytecodeChunkSourcePath(&chunk, path);
    bool used_cache = 0;
    if (!no_cache) used_cache = loadBytecodeFromCache(path, kReaCompilerId, argv[0], dep_array, dep_count, &chunk);
    if (dep_array) free(dep_array);
    freeList(dep_files);
    if (used_cache) {
#if defined(__APPLE__)
#define PSCAL_STAT_SEC(st) ((st).st_mtimespec.tv_sec)
#else
#define PSCAL_STAT_SEC(st) ((st).st_mtim.tv_sec)
#endif
        char* cache_path = buildCachePath(path, kReaCompilerId);
        struct stat cache_stat;
        if (!cache_path || stat(cache_path, &cache_stat) != 0 ||
            !importsAreFresh(program, PSCAL_STAT_SEC(cache_stat))) {
            if (cache_path) free(cache_path);
            freeBytecodeChunk(&chunk);
            initBytecodeChunk(&chunk);
            used_cache = false;
        } else {
            free(cache_path);
        }
#undef PSCAL_STAT_SEC
    }

    InterpretResult result = INTERPRET_COMPILE_ERROR;
    bool compilation_ok = true;
    if (!used_cache) {
        int moduleCount = PSCAL_FRONTEND_GET_LOADED_MODULE_COUNT();
        for (int i = 0; i < moduleCount && compilation_ok; i++) {
            AST *moduleAST = PSCAL_FRONTEND_GET_MODULE_AST(i);
            if (!moduleAST) continue;
            annotateTypes(moduleAST, NULL, moduleAST);
            if (!compileModuleAST(moduleAST, &chunk)) {
                compilation_ok = false;
                fprintf(stderr, "Compilation failed while processing module '%s'.\n",
                        PSCAL_FRONTEND_GET_MODULE_NAME(i)
                            ? PSCAL_FRONTEND_GET_MODULE_NAME(i)
                            : PSCAL_FRONTEND_GET_MODULE_PATH(i));
            }
        }

        // Annotate types for the entire program prior to compilation so that
        // qualified method calls can be resolved to their class-mangled routines.
        if (compilation_ok) {
            annotateTypes(program, NULL, program);
            compilerEnableDynamicLocals(1);
            compilation_ok = compileASTToBytecode(program, &chunk);
            compilerEnableDynamicLocals(0);
        }
        if (compilation_ok) {
            finalizeBytecode(&chunk);
            saveBytecodeToCache(path, kReaCompilerId, &chunk);
            if (verbose_flag) {
                fprintf(stderr, "Compilation successful. Bytecode size: %d bytes, Constants: %d\n",
                        chunk.count, chunk.constants_count);
            }
            if (dump_bytecode_flag) {
                fprintf(stderr, "--- Compiling Main Program AST to Bytecode ---\n");
                const char* disasm_name = path ? bytecodeDisplayNameForPath(path) : "CompiledChunk";
                disassembleBytecodeChunk(&chunk, disasm_name, procedure_table);
                if (dump_bytecode_only_flag) {
                    _exit(EXIT_SUCCESS);
                } else if (!no_run_flag) {
                    fprintf(stderr, "\n--- executing Program with VM ---\n");
                }
            }
        } else {
            fprintf(stderr, "Compilation failed with errors.\n");
        }
    } else {
        if (verbose_flag) {
            fprintf(stderr, "Loaded cached bytecode. Bytecode size: %d bytes, Constants: %d\n",
                    chunk.count, chunk.constants_count);
        }
        if (dump_bytecode_flag) {
            const char* disasm_name = path ? bytecodeDisplayNameForPath(path) : "CompiledChunk";
            disassembleBytecodeChunk(&chunk, disasm_name, procedure_table);
            if (dump_bytecode_only_flag) {
                _exit(EXIT_SUCCESS);
            } else if (!no_run_flag) {
                fprintf(stderr, "\n--- executing Program with VM (cached) ---\n");
            }
        }
    }

    diagnosticCaptureEnd(&diagCapture);
    if (!compilation_ok && (diagnostics_json || diagnostics_toon)) {
        char *diagText = diagnosticCaptureRead(&diagCapture);
        emitDiagnosticsFromText(stderr, diagText, diagnostics_toon);
        free(diagText);
    }
    diagnosticCaptureDispose(&diagCapture);

    if (compilation_ok) {
        if (argc > argi) {
            gParamCount = argc - argi;
            gParamValues = &argv[argi];
        }

        if (dump_bytecode_only_flag || no_run_flag) {
            result = INTERPRET_OK;
        } else {
            DiagnosticCapture runtimeDiagCapture = {
                .enabled = diagnostics_json || diagnostics_toon,
                .saved_stderr_fd = -1,
                .tmp = NULL
            };
            if (runtimeDiagCapture.enabled && !diagnosticCaptureBegin(&runtimeDiagCapture)) {
                fprintf(stderr, "Failed to initialize runtime diagnostics capture: %s\n", strerror(errno));
                freeBytecodeChunk(&chunk);
                freeAST(program);
                freeProcedureTable();
                if (preprocessed_source) free(preprocessed_source);
                free(src);
                REA_RETURN(vmExitWithCleanup(EXIT_FAILURE));
            }
            reaInstallSigint();
            VM vm;
            initVM(&vm);
            if (vm_trace_head > 0) vm.trace_head_instructions = vm_trace_head;
            // Inline trace toggle via comment directives: trace on/off inside source
            if (!vm_trace_head && ((preprocessed_source && strstr(preprocessed_source, "trace on")) ||
                                    (src && strstr(src, "trace on")))) {
                vm.trace_head_instructions = 16;
            }
            g_sigint_vm = &vm;
            result = interpretBytecode(&vm, &chunk, globalSymbols, constGlobalSymbols, procedure_table, 0);
            g_sigint_vm = NULL;
            freeVM(&vm);
            if (runtimeDiagCapture.enabled) {
                diagnosticCaptureEnd(&runtimeDiagCapture);
                if (result != INTERPRET_OK) {
                    char *diagText = diagnosticCaptureRead(&runtimeDiagCapture);
                    emitRuntimeDiagnosticsFromText(stderr, diagText, path, diagnostics_toon);
                    free(diagText);
                }
                diagnosticCaptureDispose(&runtimeDiagCapture);
            }
        }
    }

    freeBytecodeChunk(&chunk);
    freeAST(program);
    freeProcedureTable();
    if (preprocessed_source) free(preprocessed_source);
    free(src);
    REA_RETURN(vmExitWithCleanup(result == INTERPRET_OK ? EXIT_SUCCESS : EXIT_FAILURE));
}
#undef REA_RETURN

#ifndef PSCAL_NO_CLI_ENTRYPOINTS
int main(int argc, char **argv) {
    return PSCAL_FRONTEND_MAIN_NAME(argc, argv);
}
#endif
