// Test CLike cast-like helpers: int, double, float, char, bool

int main() {
  // int(real)
  printf("%d\n", toint(3.7));        // 3
  printf("%d\n", toint(-3.7));       // -3 (truncate toward zero)

  // int(double(int)) round trip
  printf("%d\n", toint(todouble(5)));  // 5
  printf("%d\n", toint(tofloat(6)));   // 6

  // char narrowing then back to int for stable compare (300 % 256 = 44)
  printf("%d\n", toint(tochar(300)));

  // bool from real/int then to int for stable compare
  printf("%d\n", toint(tobool(0.0)));
  printf("%d\n", toint(tobool(2)));
  return 0;
}

