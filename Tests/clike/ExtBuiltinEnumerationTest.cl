int main() {
  int count = extbuiltincategorycount();
  int ok = 1;
  int foundSystem = 0;
  int foundGetPid = 0;
  int i = 0;
  while (i < count) {
    str name = extbuiltincategoryname(i);
    if (name == "") {
      ok = 0;
    }
    int fnCount = extbuiltinfunctioncount(name);
    int j = 0;
    while (j < fnCount) {
      str fnName = extbuiltinfunctionname(name, j);
      if (fnName == "") {
        ok = 0;
      }
      if (name == "system" && fnName == "GetPid") {
        foundGetPid = 1;
      }
      j = j + 1;
    }
    if (name == "system") {
      foundSystem = 1;
    }
    i = i + 1;
  }
  if (hasextbuiltin("system", "GetPid")) {
    if (!(foundSystem && foundGetPid)) {
      ok = 0;
    }
  }
  if (ok) {
    printf("ok\n");
    return 0;
  }
  printf("fail\n");
  return 1;
}
