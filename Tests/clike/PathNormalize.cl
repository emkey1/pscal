// Unit-test style check for path normalization logic used by simple_web_server.

str normalizePath(str s) {
  str out = "";
  int i = 1;
  while (i <= length(s)) {
    while (i <= length(s) && copy(s, i, 1) == "/") i = i + 1;
    if (i > length(s)) break;
    int j = i;
    while (j <= length(s) && copy(s, j, 1) != "/") j = j + 1;
    int segLen = j - i;
    str seg = copy(s, i, segLen);
    if (seg == "" || seg == ".") {
      // skip
    } else if (seg == "..") {
      if (length(out) > 0) {
        int k = 1; int last = 0;
        while (k <= length(out)) { if (copy(out, k, 1) == "/") last = k; k = k + 1; }
        if (last > 0) out = copy(out, 1, last - 1); else out = "";
      }
    } else {
      out = (length(out) > 0) ? out + "/" + seg : seg;
    }
    i = j + 1;
  }
  return out;
}

int main() {
  printf("%s\n", normalizePath("index.html"));
  printf("%s\n", normalizePath("a//b///c"));
  printf("%s\n", normalizePath("./a/b"));
  printf("%s\n", normalizePath("a/./b/./c"));
  printf("%s\n", normalizePath("..//..//secret"));
  printf("%s\n", normalizePath("/../etc/passwd"));
  printf("%s\n", normalizePath("/./a//../b/./../c"));
  return 0;
}
