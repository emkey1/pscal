int main() {
    int db;
    int stmt;
    int rc;

    db = SqliteOpen(":memory:");
    if (db < 0) {
        printf("open_failed\n");
        return 1;
    }
    printf("open_ok\n");

    rc = SqliteExec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT);");
    printf("create_rc=%d\n", rc);

    stmt = SqlitePrepare(db, "INSERT INTO t(name) VALUES (?1);");
    printf("insert_stmt_valid=%s\n", stmt >= 0 ? "true" : "false");

    rc = SqliteBindText(stmt, 1, "alpha");
    printf("bind_alpha_rc=%d\n", rc);
    rc = SqliteStep(stmt);
    printf("insert_alpha_step=%d\n", rc);
    rc = SqliteReset(stmt);
    printf("insert_reset_rc=%d\n", rc);

    rc = SqliteBindText(stmt, 1, "beta");
    printf("bind_beta_rc=%d\n", rc);
    rc = SqliteStep(stmt);
    printf("insert_beta_step=%d\n", rc);
    rc = SqliteFinalize(stmt);
    printf("insert_finalize_rc=%d\n", rc);

    printf("last_rowid=%lld\n", (long long)SqliteLastInsertRowId(db));
    printf("changes=%d\n", SqliteChanges(db));

    stmt = SqlitePrepare(db, "SELECT id, name FROM t ORDER BY id;");
    printf("select_stmt_valid=%s\n", stmt >= 0 ? "true" : "false");

    rc = SqliteStep(stmt);
    printf("select_step1=%d\n", rc);
    printf("col_count=%d\n", SqliteColumnCount(stmt));
    printf("col0_type=%s\n", SqliteColumnType(stmt, 0));
    printf("col1_type=%s\n", SqliteColumnType(stmt, 1));
    printf("col0_int=%lld\n", (long long)SqliteColumnInt(stmt, 0));
    printf("col1_text=%s\n", SqliteColumnText(stmt, 1));

    rc = SqliteStep(stmt);
    printf("select_step2=%d\n", rc);
    printf("col0_int2=%lld\n", (long long)SqliteColumnInt(stmt, 0));
    printf("col1_text2=%s\n", SqliteColumnText(stmt, 1));

    rc = SqliteStep(stmt);
    printf("select_step3=%d\n", rc);

    rc = SqliteFinalize(stmt);
    printf("select_finalize_rc=%d\n", rc);

    printf("close_rc=%d\n", SqliteClose(db));
    return 0;
}
