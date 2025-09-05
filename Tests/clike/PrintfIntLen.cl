int main() {
  // Validate integer length modifiers map and cast correctly.
  // Use small values to be portable across 32/64-bit (no overflow).
  printf("%ld %jd %zu %llu %hhd\n", 42, 43, 44, 45, -1);
  return 0;
}

