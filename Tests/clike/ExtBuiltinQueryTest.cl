int main() {
  int pid = getpid();
  if (pid <= 0) {
    printf("fail\n");
    return 1;
  }
  if (hasextbuiltin("system", "GetPid") && !hasextbuiltin("system", "NoSuchFunction")) {
    printf("ok\n");
    return 0;
  }
  printf("fail\n");
  return 1;
}
