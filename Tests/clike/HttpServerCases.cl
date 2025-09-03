int main() {
  int s = httpsession();
  if (s < 0) { printf("ERR\n"); return 0; }

  // Test headers endpoint
  mstream h = mstreamcreate();
  int code = httprequest(s, "GET", "http://127.0.0.1:8081/headers", NULL, h);
  printf("H %d %s\n", code, httpgetheader(s, "X-Test"));
  mstreamfree(&h);

  // Test redirect without following
  httpsetoption(s, "follow_redirects", 0);
  mstream r = mstreamcreate();
  code = httprequest(s, "GET", "http://127.0.0.1:8081/redirect", NULL, r);
  printf("R %d\n", code);
  mstreamfree(&r);
  httpsetoption(s, "follow_redirects", 1);

  // Test error code
  mstream e = mstreamcreate();
  code = httprequest(s, "GET", "http://127.0.0.1:8081/notfound", NULL, e);
  printf("E %d\n", code);
  mstreamfree(&e);

  httpclose(s);
  return 0;
}
