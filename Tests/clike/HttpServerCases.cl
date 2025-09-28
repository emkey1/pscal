int main() {
  int s = httpsession();
  if (s < 0) { printf("ERR\n"); return 0; }

  str port = getenv("CLIKE_HTTP_TEST_PORT");
  if (length(port) == 0) { port = "8081"; }
  str base = "http://127.0.0.1:" + port;

  // Test headers endpoint
  mstream h = mstreamcreate();
  int code = httprequest(s, "GET", base + "/headers", NULL, h);
  printf("H %d %s\n", code, httpgetheader(s, "X-Test"));
  mstreamfree(&h);

  // Test redirect without following
  httpsetoption(s, "follow_redirects", 0);
  mstream r = mstreamcreate();
  code = httprequest(s, "GET", base + "/redirect", NULL, r);
  printf("R %d\n", code);
  mstreamfree(&r);
  httpsetoption(s, "follow_redirects", 1);

  // Test error code
  mstream e = mstreamcreate();
  code = httprequest(s, "GET", base + "/notfound", NULL, e);
  printf("E %d\n", code);
  mstreamfree(&e);

  httpclose(s);
  return 0;
}
