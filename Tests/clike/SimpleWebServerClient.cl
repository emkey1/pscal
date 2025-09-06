int read_port() {
  str pf = "/tmp/pscal_sws_port";
  if (!fileexists(pf)) return 5577;
  mstream ms = mstreamcreate();
  int ok = mstreamloadfromfile(&ms, pf);
  if (ok == 0) { mstreamfree(&ms); return 5577; }
  str s = mstreambuffer(ms);
  int p = atoi(s);
  mstreamfree(&ms);
  if (p <= 0) p = 5577;
  return p;
}

int PORT = read_port();

int send_request(int port, str path, str expect_substr, str expect_status) {
  int s = socketcreate(0);
  if (s < 0) { printf("ERR: socketcreate\n"); return 1; }
  if (socketconnect(s, "127.0.0.1", port) != 0) { printf("ERR: connect\n"); socketclose(s); return 1; }
  str req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
  socketsend(s, req);
  mstream ms = socketreceive(s, 4096);
  str resp = mstreambuffer(ms);
  socketclose(s);
  mstreamfree(&ms);
  if (pos(resp, expect_status) > 0 && pos(resp, expect_substr) > 0) return 0;
  return 1;
}

int main() {
  // 1) index.html returns 200 and contains greeting
  if (send_request(PORT, "/index.html", "Hello from the CLike web server", "200 OK") == 0) printf("OK1\n");
  else { printf("FAIL1\n"); return 1; }

  // 2) File with space in name using percent-encoding
  if (send_request(PORT, "/file%20name.txt", "hello-file", "200 OK") == 0) printf("OK2\n");
  else { printf("FAIL2\n"); return 1; }

  // 3) Traversal attempt should not succeed
  if (send_request(PORT, "/%2e%2e/secret", "Not Found", "404 Not Found") == 0) printf("OK3\n");
  else { printf("FAIL3\n"); return 1; }

  return 0;
}
