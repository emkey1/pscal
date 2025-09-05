int ready = 0;

void server() {
  int port = 54333;
  int srv = socketcreate(0);
  int rc = socketbindaddr(srv, "127.0.0.1", port);
  if (rc != 0) {
    printf("binderr:%d\n", socketlasterror());
    return;
  }
  socketlisten(srv, 1);
  ready = 1;
  int conn = socketaccept(srv);
  mstream ms = socketreceive(conn, 1024);
  socketsend(conn, ms);
  mstreamfree(&ms);
  socketclose(conn);
  socketclose(srv);
}

int main() {
  int tid = spawn server();
  while (!ready) delay(10);
  int s = socketcreate(0);
  socketconnect(s, "127.0.0.1", 54333);
  socketsend(s, "bindaddr-ok");
  mstream resp = socketreceive(s, 1024);
  printf("%s\n", mstreambuffer(resp));
  mstreamfree(&resp);
  socketclose(s);
  join tid;
  return 0;
}

