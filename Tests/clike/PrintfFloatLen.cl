int main() {
  // Verify that %Lf is handled without narrowing and prints correctly.
  printf("%.2Lf\n", 3.1415926535897932384626);
  // Also print with default %f to compare.
  printf("%.2f\n", 3.1415926535897932384626);
  return 0;
}

