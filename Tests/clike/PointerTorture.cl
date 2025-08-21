int main() {
    printf("Running CLike Pointer Torture Test\n");

    // --- Testing Integer Pointers ---
    int* intPtr1;
    int* intPtr2;
    int* intPtrNil;

    // 1. Initialization and Null Checks
    intPtr1 = NULL;
    intPtr2 = NULL;
    intPtrNil = NULL;
    if (intPtr1 == NULL) printf("START: intPtr1 initialized to NULL: PASS\n"); else printf("START: intPtr1 initialized to NULL: FAIL\n");
    if (intPtr2 == NULL) printf("START: intPtr2 initialized to NULL: PASS\n"); else printf("START: intPtr2 initialized to NULL: FAIL\n");
    if (intPtr1 == intPtrNil) printf("START: intPtr1 == intPtrNil (NULL comparison): PASS\n"); else printf("START: intPtr1 == intPtrNil (NULL comparison): FAIL\n");

    // 2. Allocation with new()
    printf("2. Allocation with new()...\n");
    new(&intPtr1);
    if (intPtr1 != NULL) printf("START: intPtr1 not NULL after new(): PASS\n"); else printf("START: intPtr1 not NULL after new(): FAIL\n");

    // 3. Assignment via Dereference
    printf("3. Assignment via Dereference...\n");
    *intPtr1 = 12345;
    if (*intPtr1 == 12345) printf("START: Assign and read *intPtr1: PASS\n"); else printf("START: Assign and read *intPtr1: FAIL\n");

    // 4. Pointer Assignment (Aliasing)
    printf("4. Pointer Assignment (Aliasing)...\n");
    intPtr2 = intPtr1;
    if (intPtr2 != NULL) printf("START: intPtr2 not NULL after assignment: PASS\n"); else printf("START: intPtr2 not NULL after assignment: FAIL\n");
    if (intPtr1 == intPtr2) printf("START: intPtr1 == intPtr2 (same address): PASS\n"); else printf("START: intPtr1 == intPtr2 (same address): FAIL\n");
    if (*intPtr2 == 12345) printf("START: Read *intPtr2 after aliasing: PASS\n"); else printf("START: Read *intPtr2 after aliasing: FAIL\n");

    // 5. Modifying through alias
    printf("5. Modifying through alias...\n");
    *intPtr2 = 54321;
    if (*intPtr2 == 54321) printf("START: Read *intPtr2 after modification: PASS\n"); else printf("START: Read *intPtr2 after modification: FAIL\n");
    if (*intPtr1 == 54321) printf("START: Read *intPtr1 after modification via intPtr2: PASS\n"); else printf("START: Read *intPtr1 after modification via intPtr2: FAIL\n");

    // 6. Disposal
    printf("6. Disposal...\n");
    dispose(&intPtr1);
    if (intPtr1 == NULL) printf("START: intPtr1 is NULL after dispose(intPtr1): PASS\n"); else printf("START: intPtr1 is NULL after dispose(intPtr1): FAIL\n");
    if (intPtr2 == NULL) printf("START: intPtr2 is NULL after disposing intPtr1: PASS\n"); else printf("START: intPtr2 is NULL after disposing intPtr1: FAIL\n");

    // 7. Disposing NULL pointer
    printf("7. Disposing NULL pointer...\n");
    if (intPtrNil == NULL) printf("START: intPtrNil is NULL before dispose: PASS\n"); else printf("START: intPtrNil is NULL before dispose: FAIL\n");
    dispose(&intPtrNil);
    if (intPtrNil == NULL) printf("START: intPtrNil is still NULL after dispose(NULL): PASS\n"); else printf("START: intPtrNil is still NULL after dispose(NULL): FAIL\n");

    // 8. Re-allocating disposed pointer
    printf("8. Re-allocating disposed pointer...\n");
    new(&intPtr1);
    if (intPtr1 != NULL) printf("START: intPtr1 not NULL after re-allocation: PASS\n"); else printf("START: intPtr1 not NULL after re-allocation: FAIL\n");
    *intPtr1 = 111;
    if (*intPtr1 == 111) printf("START: Assign/read after re-allocation: PASS\n"); else printf("START: Assign/read after re-allocation: FAIL\n");
    dispose(&intPtr1);
    if (intPtr1 == NULL) printf("START: intPtr1 is NULL after final dispose: PASS\n"); else printf("START: intPtr1 is NULL after final dispose: FAIL\n");

    // --- Testing String Pointers ---
    printf("\n--- Testing String Pointers ---\n");
    str* strPtr1;
    str* strPtr2;

    strPtr1 = NULL;
    if (strPtr1 == NULL) printf("START: strPtr1 initialized to NULL: PASS\n"); else printf("START: strPtr1 initialized to NULL: FAIL\n");

    new(&strPtr1);
    if (strPtr1 != NULL) printf("START: strPtr1 not NULL after new(): PASS\n"); else printf("START: strPtr1 not NULL after new(): FAIL\n");
    if (*strPtr1 == "") printf("START: strPtr1 initial value: PASS\n"); else printf("START: strPtr1 initial value: FAIL\n");

    *strPtr1 = "Hello";
    if (*strPtr1 == "Hello") printf("START: Assign/read strPtr1: PASS\n"); else printf("START: Assign/read strPtr1: FAIL\n");

    strPtr2 = strPtr1; // Aliasing
    *strPtr2 = *strPtr2 + ", World!";
    if (*strPtr1 == "Hello, World!") printf("START: Read strPtr1 after modification via strPtr2: PASS\n"); else printf("START: Read strPtr1 after modification via strPtr2: FAIL\n");

    dispose(&strPtr1);
    if (strPtr1 == NULL) printf("START: strPtr1 is NULL after dispose: PASS\n"); else printf("START: strPtr1 is NULL after dispose: FAIL\n");

    // --- Testing Record Pointers ---
    printf("\n--- Testing Record Pointers ---\n");
    struct Rec { int id; str label; float value; };
    struct Rec* recPtrA;
    struct Rec* recPtrB;
    struct Rec* recPtrNil;

    recPtrA = NULL;
    recPtrB = NULL;
    recPtrNil = NULL;
    if (recPtrA == NULL) printf("START: recPtrA initialized to NULL: PASS\n"); else printf("START: recPtrA initialized to NULL: FAIL\n");
    if (recPtrA == recPtrNil) printf("START: recPtrA == recPtrNil: PASS\n"); else printf("START: recPtrA == recPtrNil: FAIL\n");

    printf("2. Allocation...\n");
    new(&recPtrA);
    if (recPtrA != NULL) printf("START: recPtrA not NULL after new(): PASS\n"); else printf("START: recPtrA not NULL after new(): FAIL\n");
    if (recPtrA->id == 0) printf("START: Initial recPtrA->id: PASS\n"); else printf("START: Initial recPtrA->id: FAIL\n");
    if (recPtrA->label == "") printf("START: Initial recPtrA->label: PASS\n"); else printf("START: Initial recPtrA->label: FAIL\n");
    if (recPtrA->value == 0.0) printf("START: Initial recPtrA->value: PASS\n"); else printf("START: Initial recPtrA->value: FAIL\n");

    printf("3. Assigning field values...\n");
    recPtrA->id = 101;
    recPtrA->label = "First Record";
    recPtrA->value = 3.14;
    if (recPtrA->id == 101) printf("START: Read recPtrA->id after assignment: PASS\n"); else printf("START: Read recPtrA->id after assignment: FAIL\n");
    if (recPtrA->label == "First Record") printf("START: Read recPtrA->label after assignment: PASS\n"); else printf("START: Read recPtrA->label after assignment: FAIL\n");
    if (recPtrA->value == 3.14) printf("START: Read recPtrA->value after assignment: PASS\n"); else printf("START: Read recPtrA->value after assignment: FAIL\n");

    printf("4. Pointer Assignment (Aliasing)...\n");
    recPtrB = recPtrA;
    if (recPtrA == recPtrB) printf("START: recPtrA == recPtrB: PASS\n"); else printf("START: recPtrA == recPtrB: FAIL\n");
    if (recPtrB->id == 101) printf("START: Read recPtrB->id via alias: PASS\n"); else printf("START: Read recPtrB->id via alias: FAIL\n");

    printf("5. Modifying via alias...\n");
    recPtrB->id = 202;
    recPtrB->label = "Modified Label";
    if (recPtrA->id == 202) printf("START: Read recPtrA->id after modification via B: PASS\n"); else printf("START: Read recPtrA->id after modification via B: FAIL\n");
    if (recPtrA->label == "Modified Label") printf("START: Read recPtrA->label after modification via B: PASS\n"); else printf("START: Read recPtrA->label after modification via B: FAIL\n");

    printf("6. Disposal...\n");
    dispose(&recPtrA);
    if (recPtrA == NULL) printf("START: recPtrA is NULL after dispose: PASS\n"); else printf("START: recPtrA is NULL after dispose: FAIL\n");
    if (recPtrB == NULL) printf("START: recPtrB is NULL after disposing recPtrA: PASS\n"); else printf("START: recPtrB is NULL after disposing recPtrA: FAIL\n");

    dispose(&recPtrNil);
    if (recPtrNil == NULL) printf("START: recPtrNil is still NULL after dispose(NULL): PASS\n"); else printf("START: recPtrNil is still NULL after dispose(NULL): FAIL\n");

    printf("\nPointer Torture Test Completed.\n");
    return 0;
}
