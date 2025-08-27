int main() {
  if (hasextbuiltin("system", "GetPid") && !hasextbuiltin("system", "NoSuchFunction")) {
    printf("ok\n");
    return 0;
  }
  printf("fail\n");
  return 1;
}
