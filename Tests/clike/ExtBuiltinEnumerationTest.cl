int main() {
  int count = extbuiltincategorycount();
  int ok = 1;
  int foundSystem = 0;
  int foundGetPid = 0;
  int foundOpenAI = 0;
  int foundOpenAIChat = 0;
  int i = 0;
  while (i < count) {
    str name = extbuiltincategoryname(i);
    if (name == "") {
      ok = 0;
    }
    int fnCount = extbuiltinfunctioncount(name);
    int groupCount = extbuiltingroupcount(name);
    int j = 0;
    int groupTotal = 0;
    while (j < groupCount) {
      str groupName = extbuiltingroupname(name, j);
      int groupFnCount = extbuiltingroupfunctioncount(name, groupName);
      int k = 0;
      groupTotal = groupTotal + groupFnCount;
      while (k < groupFnCount) {
        str fnName = extbuiltingroupfunctionname(name, groupName, k);
        if (fnName == "") {
          ok = 0;
        }
        if (name == "system" && groupName == "process" && fnName == "GetPid") {
          foundGetPid = 1;
        }
        if (name == "openai" && groupName == "chat" && fnName == "OpenAIChatCompletions") {
          foundOpenAI = 1;
          foundOpenAIChat = 1;
        }
        k = k + 1;
      }
      j = j + 1;
    }
    if (groupTotal != fnCount) {
      ok = 0;
    }
    if (name == "system") {
      foundSystem = 1;
    }
    if (name == "openai") {
      foundOpenAI = 1;
    }
    i = i + 1;
  }
  if (hasextbuiltin("system", "GetPid")) {
    if (!(foundSystem && foundGetPid)) {
      ok = 0;
    }
  }
  if (hasextbuiltin("openai", "OpenAIChatCompletions")) {
    if (!(foundOpenAI && foundOpenAIChat)) {
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
