// Additional coverage for integer printf length modifiers.
int main() {
  printf("%hd %hhd\n", 1234, 23);
  printf("%jd\n", (long long)-123456789);
  printf("%hhu\n", 255);
  printf("%zu %td\n", 44, -5);
  return 0;
}

