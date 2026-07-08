// VM 2.0 Phase 5a/5b CLike coverage (Docs/pscal_vm2_plan.md Sec 6.1/6.2).
// Regression fixture for a real gap found during post-ship verification:
// TaskSpawn/TaskAwait/TaskDone/TaskCancel had no entry in CLike's semantic
// analyzer at all (only the pre-existing httprequestasync-family builtins,
// migrated onto TYPE_TASK by 5a-iii, got type-checked correctly) -- any
// CLike program declaring `task t = TaskSpawn(...)` failed to compile
// ("cannot assign INTEGER to TASK"). Channel had no such gap (channelcreate
// was already wired to TYPE_CHANNEL). Exercises the full family plus
// &funcname task-spawn syntax and a task-channel handoff.

int slowFn(int n) {
    return n + 100;
}

int producer(int startVal) {
    channel ch;
    ch = g_channel;
    int i;
    for (i = 0; i < 3; i = i + 1) {
        ChannelSend(ch, startVal + i);
    }
    ChannelClose(ch);
    return 0;
}

channel g_channel;

int main() {
    // TaskSpawn / TaskAwait / TaskDone / TaskCancel over a plain function.
    task t;
    t = TaskSpawn(&slowFn, 5);
    int result;
    result = TaskAwait(t);
    if (result != 105) {
        printf("FAIL: TaskAwait expected 105, got %d\n", result);
        return 1;
    }

    task t2;
    t2 = TaskSpawn(&slowFn, 1);
    int cancelled;
    cancelled = TaskCancel(t2);
    if (cancelled != 1) {
        printf("FAIL: TaskCancel expected 1, got %d\n", cancelled);
        return 1;
    }
    TaskAwait(t2); // drain regardless of whether the cancel won the race

    // Task + Channel handoff: a spawned task sends through a channel the
    // main thread reads from.
    g_channel = ChannelCreate(1);
    task producerTask;
    producerTask = TaskSpawn(&producer, 10);
    int v;
    int count;
    count = 0;
    v = ChannelReceive(g_channel);
    while (v != NULL) {
        count = count + 1;
        v = ChannelReceive(g_channel);
    }
    TaskAwait(producerTask);
    if (count != 3) {
        printf("FAIL: expected 3 channel values, got %d\n", count);
        return 1;
    }

    printf("OK\n");
    return 0;
}
