// Test that mixed-case function names resolve case-insensitively

int FooBar(int x) {
  return x + 1;
}

int main() {
  // Call with different casing than the declaration
  printf("%d\n", foobar(41));  // expect 42
  printf("%d\n", FOOBAR(1));   // expect 2
  return 0;
}

