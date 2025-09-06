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

int PORT;
int mu;
int successes = 0;

int send_request() {
  int s = socketcreate(0);
  if (s < 0) return 1;
  if (socketconnect(s, "127.0.0.1", PORT) != 0) { socketclose(s); return 1; }
  str req = "GET /index.html HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
  socketsend(s, req);
  mstream ms = socketreceive(s, 4096);
  str resp = mstreambuffer(ms);
  socketclose(s);
  mstreamfree(&ms);
  if (pos(resp, "200 OK") > 0 && pos(resp, "Hello from the CLike web server") > 0) return 0;
  return 1;
}

void worker() {
  if (send_request() == 0) {
    lock(mu);
    successes = successes + 1;
    unlock(mu);
  }
}

int main() {
  PORT = 5555;
  int n = 16;
  if (paramcount() >= 1) {
    str a1 = paramstr(1);
    int parsed = 0; int code = -1;
    val(a1, &parsed, &code);
    if (code == 0 && parsed > 0 && parsed < 1024) n = parsed;
  }
  mu = mutex();
  int tids[1024];
  int i = 0;
  while (i < n) { tids[i] = spawn worker(); i = i + 1; }
  i = 0;
  while (i < n) { join tids[i]; i = i + 1; }
  destroy(mu);
  printf("Done %d/%d\n", successes, n);
  if (successes == n) {
    printf("OK\n");
    return 0;
  }
  printf("FAIL\n");
  return 1;
}
