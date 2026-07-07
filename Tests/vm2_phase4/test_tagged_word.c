// Tests/vm2_phase4/test_tagged_word.c
//
// Standalone unit tests for VM 2.0 Phase 4 Stage A sub-phase 4h
// (Docs/pscal_vm2_plan.md sections 5.10.1/5.10.4/5.10.8): the actual
// immediate/pointer tagged-word bit-packing built on top of 4a's
// NaN-boxing foundation, plus Int64Box/LongDoubleBox for the two numeric
// types that must stay heap-boxed. All in isolation -- nothing in the VM
// depends on any of this yet (Value's physical layout doesn't change
// until 4i). Built and run by Tests/run_vm2_phase4_tests.sh, alongside
// 4a's test_obj_header.c.

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/obj_header.h"
#include "core/var_type.h"

// ---- Pointer tagging ----

static void test_pointer_round_trip(void) {
    int stack_var = 42;
    assert(pscalObjPointerFitsPayload(&stack_var));
    uint64_t word = pscalTagPointer(&stack_var);
    assert(pscalWordIsNanBoxTag(word));
    assert(pscalTaggedWordIsPointer(word));
    void *back = pscalUntagPointer(word);
    assert(back == (void *)&stack_var);

    void *heap_ptr = malloc(64);
    assert(heap_ptr);
    assert(pscalObjPointerFitsPayload(heap_ptr));
    uint64_t hword = pscalTagPointer(heap_ptr);
    assert(pscalWordIsNanBoxTag(hword));
    assert(pscalTaggedWordIsPointer(hword));
    assert(pscalUntagPointer(hword) == heap_ptr);
    free(heap_ptr);

    // NULL is a legitimate pointer payload (kind-less nil-address case;
    // Value's actual NIL representation uses PSCAL_TAG_NIL instead, but
    // the pointer path itself must not treat 0 specially).
    uint64_t nullword = pscalTagPointer(NULL);
    assert(pscalWordIsNanBoxTag(nullword));
    assert(pscalTaggedWordIsPointer(nullword));
    assert(pscalUntagPointer(nullword) == NULL);

    printf("test_pointer_round_trip: PASS\n");
}

static void test_pointer_and_immediate_words_are_disjoint(void) {
    // A pointer word and an immediate word must never be mistaken for
    // each other -- this is the entire reason bit 50 exists as a
    // dedicated discriminant rather than overloading the kind field.
    int x = 1;
    uint64_t pword = pscalTagPointer(&x);
    uint64_t iword = pscalTagInt32(12345);
    assert(pscalTaggedWordIsPointer(pword));
    assert(!pscalTaggedWordIsPointer(iword));
    printf("test_pointer_and_immediate_words_are_disjoint: PASS\n");
}

// ---- Immediate scalar round-trips ----

static void test_void_nil(void) {
    uint64_t vw = pscalTagVoid();
    assert(pscalWordIsNanBoxTag(vw));
    assert(!pscalTaggedWordIsPointer(vw));
    assert(pscalTaggedWordKind(vw) == PSCAL_TAG_VOID);
    assert(pscalTaggedWordPayload(vw) == 0);

    uint64_t nw = pscalTagNil();
    assert(pscalTaggedWordKind(nw) == PSCAL_TAG_NIL);
    assert(pscalTaggedWordPayload(nw) == 0);
    assert(vw != nw); // distinct kinds must not collide

    printf("test_void_nil: PASS\n");
}

static void test_boolean_round_trip(void) {
    uint64_t t = pscalTagBoolean(true);
    uint64_t f = pscalTagBoolean(false);
    assert(pscalTaggedWordKind(t) == PSCAL_TAG_BOOLEAN);
    assert(pscalTaggedWordKind(f) == PSCAL_TAG_BOOLEAN);
    assert(pscalUntagBoolean(t) == true);
    assert(pscalUntagBoolean(f) == false);
    printf("test_boolean_round_trip: PASS\n");
}

static void test_char_round_trip(void) {
    char samples[] = {'A', 'z', '0', '\0', (char)0x7F};
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        uint64_t w = pscalTagChar(samples[i]);
        assert(pscalTaggedWordKind(w) == PSCAL_TAG_CHAR);
        assert(pscalUntagChar(w) == samples[i]);
    }
    printf("test_char_round_trip: PASS\n");
}

static void test_widechar_round_trip(void) {
    uint32_t samples[] = {0, 0x41, 0x1F600 /* an emoji codepoint */, 0x10FFFF /* max valid codepoint */};
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        uint64_t w = pscalTagWideChar(samples[i]);
        assert(pscalTaggedWordKind(w) == PSCAL_TAG_WIDECHAR);
        assert(pscalUntagWideChar(w) == samples[i]);
    }
    printf("test_widechar_round_trip: PASS\n");
}

static void test_byte_word_round_trip(void) {
    uint8_t byte_samples[] = {0, 1, 127, 128, 255};
    for (size_t i = 0; i < sizeof(byte_samples) / sizeof(byte_samples[0]); i++) {
        uint64_t w = pscalTagByte(byte_samples[i]);
        assert(pscalTaggedWordKind(w) == PSCAL_TAG_BYTE);
        assert(pscalUntagByte(w) == byte_samples[i]);
    }
    uint16_t word_samples[] = {0, 1, 32767, 32768, 65535};
    for (size_t i = 0; i < sizeof(word_samples) / sizeof(word_samples[0]); i++) {
        uint64_t w = pscalTagWord(word_samples[i]);
        assert(pscalTaggedWordKind(w) == PSCAL_TAG_WORD);
        assert(pscalUntagWord(w) == word_samples[i]);
    }
    printf("test_byte_word_round_trip: PASS\n");
}

static void test_signed_extension_boundaries(void) {
    // The load-bearing case this test exists for: negative values must
    // survive the pack/unpack round trip with their sign intact, not get
    // zero-extended into a huge positive number.
    int8_t i8_samples[] = {INT8_MIN, -1, 0, 1, INT8_MAX};
    for (size_t i = 0; i < sizeof(i8_samples) / sizeof(i8_samples[0]); i++) {
        uint64_t w = pscalTagInt8(i8_samples[i]);
        assert(pscalTaggedWordKind(w) == PSCAL_TAG_INT8);
        assert(pscalUntagInt8(w) == i8_samples[i]);
    }
    int16_t i16_samples[] = {INT16_MIN, -1, 0, 1, INT16_MAX};
    for (size_t i = 0; i < sizeof(i16_samples) / sizeof(i16_samples[0]); i++) {
        uint64_t w = pscalTagInt16(i16_samples[i]);
        assert(pscalTaggedWordKind(w) == PSCAL_TAG_INT16);
        assert(pscalUntagInt16(w) == i16_samples[i]);
    }
    int32_t i32_samples[] = {INT32_MIN, -1, 0, 1, INT32_MAX};
    for (size_t i = 0; i < sizeof(i32_samples) / sizeof(i32_samples[0]); i++) {
        uint64_t w = pscalTagInt32(i32_samples[i]);
        assert(pscalTaggedWordKind(w) == PSCAL_TAG_INT32);
        assert(pscalUntagInt32(w) == i32_samples[i]);
    }
    printf("test_signed_extension_boundaries: PASS\n");
}

static void test_unsigned_boundaries(void) {
    uint8_t u8_samples[] = {0, 1, 254, 255};
    for (size_t i = 0; i < sizeof(u8_samples) / sizeof(u8_samples[0]); i++) {
        uint64_t w = pscalTagUInt8(u8_samples[i]);
        assert(pscalUntagUInt8(w) == u8_samples[i]);
    }
    uint16_t u16_samples[] = {0, 1, 65534, 65535};
    for (size_t i = 0; i < sizeof(u16_samples) / sizeof(u16_samples[0]); i++) {
        uint64_t w = pscalTagUInt16(u16_samples[i]);
        assert(pscalUntagUInt16(w) == u16_samples[i]);
    }
    uint32_t u32_samples[] = {0, 1, 4294967294u, 4294967295u};
    for (size_t i = 0; i < sizeof(u32_samples) / sizeof(u32_samples[0]); i++) {
        uint64_t w = pscalTagUInt32(u32_samples[i]);
        assert(pscalTaggedWordKind(w) == PSCAL_TAG_UINT32);
        assert(pscalUntagUInt32(w) == u32_samples[i]);
    }
    printf("test_unsigned_boundaries: PASS\n");
}

static void assert_float_round_trips(float f) {
    uint64_t w = pscalTagFloat(f);
    assert(pscalWordIsNanBoxTag(w));
    assert(pscalTaggedWordKind(w) == PSCAL_TAG_FLOAT);
    float back = pscalUntagFloat(w);
    if (isnan(f)) {
        assert(isnan(back));
    } else {
        uint32_t fb, bb;
        memcpy(&fb, &f, sizeof(fb));
        memcpy(&bb, &back, sizeof(bb));
        assert(fb == bb);
    }
}

static void test_float_round_trip(void) {
    assert_float_round_trips(0.0f);
    assert_float_round_trips(-0.0f);
    assert_float_round_trips(1.5f);
    assert_float_round_trips(-123456.75f);
    assert_float_round_trips(3.14159265f);
    assert_float_round_trips(1e30f);
    assert_float_round_trips(INFINITY);
    assert_float_round_trips(-INFINITY);
    // A float NaN packed as FLOAT-kind payload does NOT need the
    // pscalBoxDouble canonicalization dance -- these 32 bits never stand
    // alone as a top-level 64-bit word, they're payload inside a word
    // whose top 14 bits (header + discriminant + kind) are already fixed
    // by pscalTagImmediate, so there is no header collision possible.
    volatile float zero = 0.0f;
    assert_float_round_trips(zero / zero);
    printf("test_float_round_trip: PASS\n");
}

static void test_all_kinds_pairwise_distinct(void) {
    // Every kind, packed with the same raw payload bits, must decode back
    // to its own kind -- confirms the 5-bit kind field and the 45-bit
    // payload field never bleed into each other.
    PscalTaggedKind kinds[] = {
        PSCAL_TAG_VOID, PSCAL_TAG_NIL, PSCAL_TAG_BOOLEAN, PSCAL_TAG_CHAR,
        PSCAL_TAG_WIDECHAR, PSCAL_TAG_BYTE, PSCAL_TAG_WORD, PSCAL_TAG_INT8,
        PSCAL_TAG_UINT8, PSCAL_TAG_INT16, PSCAL_TAG_UINT16, PSCAL_TAG_INT32,
        PSCAL_TAG_UINT32, PSCAL_TAG_FLOAT,
    };
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        uint64_t w = pscalTagImmediate(kinds[i], 0x1A2B3Cu);
        assert(pscalWordIsNanBoxTag(w));
        assert(!pscalTaggedWordIsPointer(w));
        assert(pscalTaggedWordKind(w) == kinds[i]);
        assert(pscalTaggedWordPayload(w) == 0x1A2B3Cu);
    }
    printf("test_all_kinds_pairwise_distinct: PASS\n");
}

static void test_immediate_payload_never_leaks_into_header(void) {
    // A payload with every one of its 45 bits set must not corrupt the
    // reserved header region or the kind field -- confirms the masking
    // in pscalTagImmediate is exact, not off-by-a-bit.
    uint64_t max_payload = PSCAL_TAG_IMMEDIATE_PAYLOAD_MASK;
    uint64_t w = pscalTagImmediate(PSCAL_TAG_UINT32, max_payload);
    assert(pscalWordIsNanBoxTag(w));
    assert(pscalTaggedWordKind(w) == PSCAL_TAG_UINT32);
    assert(pscalTaggedWordPayload(w) == max_payload);
    printf("test_immediate_payload_never_leaks_into_header: PASS\n");
}

// ---- Int64Box / LongDoubleBox ----

static void test_int64_box_lifecycle(void) {
    Int64Box *box = pscalInt64BoxCreate(-9000000000000000000LL);
    assert(box);
    assert(atomic_load(&box->header.refcount) == 1);
    assert(box->header.type == TYPE_INT64);
    assert(box->value == -9000000000000000000LL);

    pscalObjRetain(&box->header);
    assert(atomic_load(&box->header.refcount) == 2);
    pscalObjRelease(&box->header);
    assert(atomic_load(&box->header.refcount) == 1);
    pscalObjRelease(&box->header); // frees box -- do not touch it again

    printf("test_int64_box_lifecycle: PASS\n");
}

static void test_int64_box_unsigned_reinterpretation(void) {
    // TYPE_UINT64's full range shares Int64Box's one int64_t field via
    // bit-pattern reinterpretation, exactly like the live VM's Value.i_val
    // already does for unsigned 64-bit values today. A UINT64 value above
    // INT64_MAX must round-trip through the shared field without loss.
    uint64_t big = 18446744073709551615ULL; // UINT64_MAX
    int64_t reinterpreted;
    memcpy(&reinterpreted, &big, sizeof(reinterpreted));
    Int64Box *box = pscalInt64BoxCreate(reinterpreted);
    box->header.type = TYPE_UINT64; // caller-selects which of the two VarTypes this instance is
    uint64_t back;
    memcpy(&back, &box->value, sizeof(back));
    assert(back == big);
    pscalObjRelease(&box->header);
    printf("test_int64_box_unsigned_reinterpretation: PASS\n");
}

static void test_long_double_box_lifecycle(void) {
    long double sample = 3.14159265358979323846L;
    LongDoubleBox *box = pscalLongDoubleBoxCreate(sample);
    assert(box);
    assert(atomic_load(&box->header.refcount) == 1);
    assert(box->header.type == TYPE_LONG_DOUBLE);
    assert(box->value == sample);

    pscalObjRelease(&box->header); // frees box
    printf("test_long_double_box_lifecycle: PASS\n");
}

int main(void) {
    test_pointer_round_trip();
    test_pointer_and_immediate_words_are_disjoint();
    test_void_nil();
    test_boolean_round_trip();
    test_char_round_trip();
    test_widechar_round_trip();
    test_byte_word_round_trip();
    test_signed_extension_boundaries();
    test_unsigned_boundaries();
    test_float_round_trip();
    test_all_kinds_pairwise_distinct();
    test_immediate_payload_never_leaks_into_header();
    test_int64_box_lifecycle();
    test_int64_box_unsigned_reinterpretation();
    test_long_double_box_lifecycle();
    printf("ALL VM 2.0 PHASE 4H UNIT TESTS PASSED\n");
    return 0;
}
