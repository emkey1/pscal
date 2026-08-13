// Tests/vm2_phase4/test_obj_header.c
//
// Standalone unit tests for VM 2.0 Phase 4 Stage A sub-phase 4a
// (Docs/pscal_vm2_plan.md sections 5.10.4/5.10.8): ObjHeader retain/
// release, NaN-box canonicalization, and the pointer-width canary -- all
// in isolation, since nothing in the VM depends on any of this yet. Built
// and run by Tests/run_vm2_phase4_tests.sh, following the same
// standalone-cc convention as Tests/ios_vproc/.

#include <assert.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core/obj_header.h"

// ---- A minimal fake heap object to exercise retain/release with ----

typedef struct FakeObj {
    ObjHeader header;
    int *destroyed_flag; // set to 1 by the destructor, so the test can see it fired
} FakeObj;

static void fakeObjDestroy(ObjHeader *header) {
    FakeObj *obj = (FakeObj *)header;
    *(obj->destroyed_flag) = 1;
    free(obj);
}

// TYPE_MEMORYSTREAM is one of the ~11 VarTypes that will eventually get a
// real ObjHeader-based shape (sub-phase 4b), but nothing in the VM
// constructs one yet as of 4a -- safe to borrow for this test's
// destructor registration without colliding with any real code path.
#define FAKE_OBJ_TYPE TYPE_MEMORYSTREAM

static void test_retain_release_lifecycle(void) {
    int destroyed = 0;
    pscalObjRegisterDestructor(FAKE_OBJ_TYPE, fakeObjDestroy);

    FakeObj *obj = malloc(sizeof(FakeObj));
    assert(obj);
    obj->destroyed_flag = &destroyed;
    pscalObjHeaderInit(&obj->header, FAKE_OBJ_TYPE);
    assert(atomic_load(&obj->header.refcount) == 1);

    ObjHeader *retained = pscalObjRetain(&obj->header);
    assert(retained == &obj->header);
    assert(atomic_load(&obj->header.refcount) == 2);

    pscalObjRetain(&obj->header);
    assert(atomic_load(&obj->header.refcount) == 3);

    pscalObjRelease(&obj->header);
    assert(destroyed == 0);
    assert(atomic_load(&obj->header.refcount) == 2);

    pscalObjRelease(&obj->header);
    assert(destroyed == 0);
    assert(atomic_load(&obj->header.refcount) == 1);

    pscalObjRelease(&obj->header);
    assert(destroyed == 1); // freed obj -- do not touch obj->header again

    printf("test_retain_release_lifecycle: PASS\n");
}

static void test_release_already_zero_refcount_is_safe(void) {
    // Simulates a double-release without actually double-freeing: force
    // the header's refcount to 0 directly (bypassing the normal release
    // path, which would already have run the destructor), then call
    // pscalObjRelease on it. It must not run the destructor a second time
    // (that would be a use-after-free layered on the original bug) and
    // must not leave the refcount corrupted (wrapped to UINT32_MAX).
    int destroyed = 0;
    FakeObj *obj = malloc(sizeof(FakeObj));
    assert(obj);
    obj->destroyed_flag = &destroyed;
    pscalObjHeaderInit(&obj->header, FAKE_OBJ_TYPE);
    atomic_store(&obj->header.refcount, 0u);

    pscalObjRelease(&obj->header);
    assert(destroyed == 0); // destructor must not have run
    assert(atomic_load(&obj->header.refcount) == 0); // restored, not left at UINT32_MAX

    free(obj); // this test owns obj's lifecycle directly, not via refcounting
    printf("test_release_already_zero_refcount_is_safe: PASS\n");
}

static void test_retain_release_null_safe(void) {
    ObjHeader *retained = pscalObjRetain(NULL);
    assert(retained == NULL);
    pscalObjRelease(NULL); // must not crash
    printf("test_retain_release_null_safe: PASS\n");
}

// ---- NaN-box canonicalization ----

static void assert_round_trips(double d) {
    uint64_t boxed = pscalBoxDouble(d);
    assert(!pscalWordIsNanBoxTag(boxed));
    double back = pscalUnboxDouble(boxed);
    if (isnan(d)) {
        assert(isnan(back));
    } else {
        // memcpy+compare-as-bits, not ==, so -0.0 is distinguished from
        // 0.0 correctly (== treats them equal; the bit pattern must still
        // round-trip exactly).
        uint64_t back_bits, d_bits;
        memcpy(&back_bits, &back, sizeof(back_bits));
        memcpy(&d_bits, &d, sizeof(d_bits));
        assert(back_bits == d_bits);
    }
}

static void test_nanbox_ordinary_doubles(void) {
    assert_round_trips(0.0);
    assert_round_trips(-0.0);
    assert_round_trips(1.0);
    assert_round_trips(-1.5);
    assert_round_trips(3.14159265358979);
    assert_round_trips(1e300);
    assert_round_trips(-1e-300);
    assert_round_trips(INFINITY);
    assert_round_trips(-INFINITY);
    printf("test_nanbox_ordinary_doubles: PASS\n");
}

static void test_nanbox_canonicalizes_colliding_nan(void) {
    // 0.0/0.0 reliably produces the canonical quiet-NaN bit pattern on
    // every targeted platform (x86_64/ARM64 IEEE-754 hardware): sign=0,
    // exponent=0x7FF, top mantissa bit=1 -- exactly PSCAL_NANBOX_HEADER.
    // This is the collision the canonicalization step exists to prevent.
    volatile double zero = 0.0;
    double colliding_nan = zero / zero;
    assert(isnan(colliding_nan));

    uint64_t raw_bits;
    memcpy(&raw_bits, &colliding_nan, sizeof(raw_bits));
    if (!pscalWordIsNanBoxTag(raw_bits)) {
        fprintf(stderr,
                "test assumption violated: 0.0/0.0 didn't produce the expected "
                "canonical QNaN bit pattern on this platform -- re-derive this "
                "test's premise before trusting the rest of the check\n");
        abort();
    }

    uint64_t boxed = pscalBoxDouble(colliding_nan);
    if (pscalWordIsNanBoxTag(boxed)) {
        fprintf(stderr,
                "pscalBoxDouble failed to move a colliding NaN out of the "
                "reserved tag region -- this is the exact corruption bug "
                "the canonicalization step exists to prevent\n");
        abort();
    }

    double back = pscalUnboxDouble(boxed);
    assert(isnan(back));

    printf("test_nanbox_canonicalizes_colliding_nan: PASS\n");
}

static void test_nanbox_header_disjoint_from_finite_and_infinity(void) {
    // Finite doubles never have exponent==0x7FF, so they can never collide
    // with the header regardless of sign/mantissa.
    assert(!pscalWordIsNanBoxTag(0x0000000000000000ULL)); // +0.0
    assert(!pscalWordIsNanBoxTag(0x8000000000000000ULL)); // -0.0
    assert(!pscalWordIsNanBoxTag(0x3FF0000000000000ULL)); // 1.0
    // +Infinity: exponent all-1, mantissa all-zero -- the header pattern
    // requires bit 51 (part of the mantissa) to be 1, so this must not
    // match.
    double pos_inf = INFINITY;
    uint64_t pos_inf_bits;
    memcpy(&pos_inf_bits, &pos_inf, sizeof(pos_inf_bits));
    assert(!pscalWordIsNanBoxTag(pos_inf_bits));
    printf("test_nanbox_header_disjoint_from_finite_and_infinity: PASS\n");
}

// ---- Pointer-width canary ----

static void test_pointer_fits_payload(void) {
    int stack_var = 0;
    assert(pscalObjPointerFitsPayload(&stack_var));

    void *heap_ptr = malloc(1);
    assert(heap_ptr);
    assert(pscalObjPointerFitsPayload(heap_ptr));
    free(heap_ptr);

    // A synthetic pointer value with bit 46 set must be rejected,
    // regardless of whether any real allocator would ever produce one.
    void *too_high = (void *)(uintptr_t)(UINT64_C(1) << 50);
    assert(!pscalObjPointerFitsPayload(too_high));

    printf("test_pointer_fits_payload: PASS\n");
}

static void test_canary_passes_under_normal_conditions(void) {
    // Must not abort: every targeted platform's allocator returns
    // pointers well under the 46-bit budget.
    pscalObjRunPointerWidthCanary();
    pscalObjRunPointerWidthCanary(); // idempotent: second call is a no-op
    printf("test_canary_passes_under_normal_conditions: PASS\n");
}

// ---- Abort-path checks, run in a forked child so a successful abort()
// doesn't take the whole test binary down with it ----

static int run_in_child_and_get_exit_status(void (*fn)(void)) {
    // Flush before forking: stdout is fully buffered (not line-buffered)
    // when this test's output is piped/captured rather than going to a
    // real TTY, so without this the child inherits whatever PASS lines
    // are still sitting unflushed in the parent's buffer and re-emits
    // them on its own exit path -- harmless to correctness (every
    // assertion still runs and is checked independently) but confusing
    // to read.
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        // Child: silence the expected diagnostic so the test's own
        // PASS/FAIL lines stay readable, then run the failing path.
        freopen("/dev/null", "w", stderr);
        fn();
        _exit(0); // only reached if fn() didn't abort -- caller treats
                  // a clean exit as "did not abort" and fails the assertion
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return status;
}

static void child_double_register(void) {
    pscalObjRegisterDestructor(TYPE_CLOSURE, fakeObjDestroy);
    pscalObjRegisterDestructor(TYPE_CLOSURE, fakeObjDestroy); // must abort
}

static void child_release_unregistered(void) {
    FakeObj *obj = malloc(sizeof(FakeObj));
    int destroyed = 0;
    obj->destroyed_flag = &destroyed;
    pscalObjHeaderInit(&obj->header, TYPE_INTERFACE); // never registered in this child
    pscalObjRelease(&obj->header); // must abort, not silently leak/misbehave
}

static void test_abort_on_double_registration(void) {
    int status = run_in_child_and_get_exit_status(child_double_register);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    printf("test_abort_on_double_registration: PASS\n");
}

static void test_abort_on_release_with_no_destructor(void) {
    int status = run_in_child_and_get_exit_status(child_release_unregistered);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    printf("test_abort_on_release_with_no_destructor: PASS\n");
}

int main(void) {
    test_retain_release_lifecycle();
    test_release_already_zero_refcount_is_safe();
    test_retain_release_null_safe();
    test_nanbox_ordinary_doubles();
    test_nanbox_canonicalizes_colliding_nan();
    test_nanbox_header_disjoint_from_finite_and_infinity();
    test_pointer_fits_payload();
    test_canary_passes_under_normal_conditions();
    test_abort_on_double_registration();
    test_abort_on_release_with_no_destructor();
    printf("ALL VM 2.0 PHASE 4A UNIT TESTS PASSED\n");
    return 0;
}
