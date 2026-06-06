int main() {
    str host = "localhost";
    printf("dns=%s\n", dnslookup(host));

    int session = httpsession();
    httpsetheader(session, "X-Name", "雪山");
    httpsetoption(session, "user_agent", "雪山");
    mstream out = mstreamcreate();
    int code = httprequest(session, "GET", "data:text/plain,%E9%9B%AA%E5%B1%B1", NULL, out);
    printf("http=%d %s\n", code, mstreambuffer(out));
    mstreamfree(&out);
    httpclose(session);

    int db = SqliteOpen(":memory:");
    printf("open=%d\n", db >= 0 ? 1 : 0);
    SqliteExec(db, "CREATE TABLE t(name TEXT);");
    int stmt = SqlitePrepare(db, "INSERT INTO t(name) VALUES (?1);");
    printf("bind=%d\n", SqliteBindText(stmt, 1, "雪山"));
    SqliteStep(stmt);
    SqliteFinalize(stmt);
    stmt = SqlitePrepare(db, "SELECT name FROM t;");
    SqliteStep(stmt);
    printf("sql=%s\n", SqliteColumnText(stmt, 0));
    SqliteFinalize(stmt);
    SqliteClose(db);

    int tid = thread_pool_submit("delay", "雪山", 1);
    WaitForThread(tid);
    printf("thread=%d\n", thread_lookup("雪山") == tid ? 1 : 0);
    return 0;
}
