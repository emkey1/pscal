int main() {
    printf("%d\n", 1 << 2 + 1);   // shift has lower precedence than + => 1 << (2+1) = 8
    printf("%d\n", 16 >> 2 + 1);  // 16 >> (2+1) = 2
    printf("%d\n", (1 << 2) + 1); // (4) + 1 = 5
    printf("%d\n", 32 >> 1 << 2); // left-associative: (32 >> 1) << 2 = 64
    return 0;
}

