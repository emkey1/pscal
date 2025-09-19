int rhs1() { printf("R1\n"); return 1; }
int rhs0() { printf("R0\n"); return 0; }

int main() {
    if (0 && rhs1()) printf("A\n"); // no RHS
    if (1 || rhs1()) printf("B\n"); // RHS skipped
    if (1 && rhs1()) printf("C\n"); // RHS evaluated
    if (0 || rhs0()) printf("D\n"); // RHS evaluated, but false so no D
    printf("vals:%d %d %d %d\n", 0 && 1, 1 || 0, 1 && 2, 0 || 5);
    return 0;
}

