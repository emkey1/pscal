int main() {
  int s = httpsession();
  if (s < 0) { printf("ERR\n"); return 0; }
  httpsetheader(s, "Accept", "text/html");
  mstream out = mstreamcreate();
  int code = httprequest(s, "GET", "https://example.com", NULL, out);
  printf("%d\n", code);
  str ctype = httpgetheader(s, "Content-Type");
  int n = length(ctype);
  if (n > 9) n = 9; // print only the media type prefix (e.g., text/html)
  for (int i = 0; i < n; i = i + 1) printf("%c", ctype[i]);
  printf("\n");
  mstreamfree(&out);
  httpclose(s);
  return 0;
}

