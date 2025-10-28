#include "backend_ast/builtin.h"
#include "core/utils.h"
#include "vm/vm.h"
#include "Pascal/globals.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int x1;
    int y1;
    int x2;
    int y2;
    int x3;
    int y3;
    int level;
    char drawChar;
} SierpinskiWorkerTask;

static pthread_mutex_t gSierpinskiMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t gSierpinskiStartMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gSierpinskiStartCond = PTHREAD_COND_INITIALIZER;
static int gSierpinskiPendingWorkers = 0;
static bool gSierpinskiWorkersReleased = false;

static void sierpinskiWorkerAwaitRelease(void) {
    pthread_mutex_lock(&gSierpinskiStartMutex);
    while (!gSierpinskiWorkersReleased) {
        pthread_cond_wait(&gSierpinskiStartCond, &gSierpinskiStartMutex);
    }
    pthread_mutex_unlock(&gSierpinskiStartMutex);
}

static void sierpinskiWorkerFinished(void) {
    pthread_mutex_lock(&gSierpinskiStartMutex);
    if (gSierpinskiPendingWorkers > 0) {
        gSierpinskiPendingWorkers--;
        if (gSierpinskiPendingWorkers == 0) {
            gSierpinskiWorkersReleased = false;
        }
    }
    pthread_mutex_unlock(&gSierpinskiStartMutex);
}

static void sierpinskiDrawPoint(VM *vm, int x, int y, char drawChar) {
    if (!vm) {
        return;
    }

    pthread_mutex_lock(&gSierpinskiMutex);

    int absX = gWindowLeft + x - 1;
    int absY = gWindowTop + y - 1;

    if (absX < 1) {
        absX = 1;
    }
    if (absY < 1) {
        absY = 1;
    }

    fprintf(stdout, "\x1B[%d;%dH%c", absY, absX, drawChar);
    fflush(stdout);

    pthread_mutex_unlock(&gSierpinskiMutex);
}

static void sierpinskiDrawRecursive(VM *vm,
                                    int x1,
                                    int y1,
                                    int x2,
                                    int y2,
                                    int x3,
                                    int y3,
                                    int level,
                                    char drawChar) {
    if (level <= 0) {
        sierpinskiDrawPoint(vm, x1, y1, drawChar);
        sierpinskiDrawPoint(vm, x2, y2, drawChar);
        sierpinskiDrawPoint(vm, x3, y3, drawChar);
        return;
    }

    int mx1 = (x1 + x2) / 2;
    int my1 = (y1 + y2) / 2;
    int mx2 = (x2 + x3) / 2;
    int my2 = (y2 + y3) / 2;
    int mx3 = (x3 + x1) / 2;
    int my3 = (y3 + y1) / 2;
    int nextLevel = level - 1;

    sierpinskiDrawRecursive(vm, x1, y1, mx1, my1, mx3, my3, nextLevel, drawChar);
    sierpinskiDrawRecursive(vm, mx1, my1, x2, y2, mx2, my2, nextLevel, drawChar);
    sierpinskiDrawRecursive(vm, mx3, my3, mx2, my2, x3, y3, nextLevel, drawChar);
}

static void sierpinskiThreadEntry(VM *threadVm, void *user_data) {
    SierpinskiWorkerTask *task = (SierpinskiWorkerTask *)user_data;
    if (!task || !threadVm) {
        return;
    }

    sierpinskiWorkerAwaitRelease();

    int level = task->level >= 0 ? task->level : 0;
    sierpinskiDrawRecursive(threadVm,
                            task->x1,
                            task->y1,
                            task->x2,
                            task->y2,
                            task->x3,
                            task->y3,
                            level,
                            task->drawChar);

    sierpinskiWorkerFinished();
}

static void sierpinskiThreadCleanup(void *user_data) {
    free(user_data);
}

static char coerceDrawChar(const Value *value) {
    if (!value) {
        return '+';
    }
    if (value->type == TYPE_CHAR) {
        return (char)value->c_val;
    }
    if (IS_INTLIKE(*value)) {
        return (char)AS_INTEGER(*value);
    }
    if (value->type == TYPE_STRING && value->s_val && value->s_val[0] != '\0') {
        return value->s_val[0];
    }
    return '+';
}

Value vmBuiltinSierpinskiSpawnWorker(VM *vm, int arg_count, Value *args) {
    if (arg_count != 7 && arg_count != 8) {
        runtimeError(vm, "SierpinskiSpawnWorker expects 7 or 8 arguments.");
        return makeInt(-1);
    }

    for (int i = 0; i < 7; ++i) {
        if (!IS_INTLIKE(args[i])) {
            runtimeError(vm, "SierpinskiSpawnWorker argument %d must be integral.", i + 1);
            return makeInt(-1);
        }
    }

    SierpinskiWorkerTask *task = (SierpinskiWorkerTask *)calloc(1, sizeof(SierpinskiWorkerTask));
    if (!task) {
        runtimeError(vm, "SierpinskiSpawnWorker failed to allocate task.");
        return makeInt(-1);
    }

    task->x1 = (int)AS_INTEGER(args[0]);
    task->y1 = (int)AS_INTEGER(args[1]);
    task->x2 = (int)AS_INTEGER(args[2]);
    task->y2 = (int)AS_INTEGER(args[3]);
    task->x3 = (int)AS_INTEGER(args[4]);
    task->y3 = (int)AS_INTEGER(args[5]);
    task->level = (int)AS_INTEGER(args[6]);
    task->drawChar = (arg_count == 8) ? coerceDrawChar(&args[7]) : '+';

    VM *targetVm = vm;
    if (targetVm && targetVm->threadOwner) {
        targetVm = targetVm->threadOwner;
    }

    pthread_mutex_lock(&gSierpinskiStartMutex);
    if (gSierpinskiPendingWorkers == 0) {
        gSierpinskiWorkersReleased = false;
    }
    pthread_mutex_unlock(&gSierpinskiStartMutex);

    int id = vmSpawnCallbackThread(targetVm, sierpinskiThreadEntry, task, sierpinskiThreadCleanup);
    if (id < 0) {
        runtimeError(vm, "SierpinskiSpawnWorker failed to spawn thread.");
        return makeInt(-1);
    }

    pthread_mutex_lock(&gSierpinskiStartMutex);
    gSierpinskiPendingWorkers++;
    pthread_mutex_unlock(&gSierpinskiStartMutex);

    return makeInt(id);
}

Value vmBuiltinSierpinskiReleaseWorkers(VM *vm, int arg_count, Value *args) {
    (void)args;

    if (arg_count != 0) {
        runtimeError(vm, "SierpinskiReleaseWorkers expects no arguments.");
        return makeVoid();
    }

    pthread_mutex_lock(&gSierpinskiStartMutex);
    gSierpinskiWorkersReleased = true;
    pthread_cond_broadcast(&gSierpinskiStartCond);
    pthread_mutex_unlock(&gSierpinskiStartMutex);

    return makeVoid();
}

void registerSierpinskiBuiltins(void) {
    registerVmBuiltin("SierpinskiSpawnWorker", vmBuiltinSierpinskiSpawnWorker,
                      BUILTIN_TYPE_FUNCTION, NULL);
    registerVmBuiltin("SierpinskiReleaseWorkers", vmBuiltinSierpinskiReleaseWorkers,
                      BUILTIN_TYPE_PROCEDURE, NULL);
}
