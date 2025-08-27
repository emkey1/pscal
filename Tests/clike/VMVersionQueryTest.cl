int main() {
  if (vmversion() == bytecodeversion())
    printf("ok\n");
  else
    printf("mismatch\n");
  return 0;
}
