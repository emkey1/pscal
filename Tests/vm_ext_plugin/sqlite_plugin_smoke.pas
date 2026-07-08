program SqlitePluginSmoke;
{ Regression fixture for Tests/vm_ext_plugin/run.sh's parity check: run
  once unmodified (the static in-tree sqlite category) and once with
  --ext pointing at sqlite_ext_plugin.dylib/.so (the standalone plugin
  build) -- both runs must produce byte-identical output. Exercises
  connection/statement/binding/column-accessor builtins so a divergence in
  any of them shows up as a diff, not just the ones a minimal smoke would
  happen to hit. }
var
  db, stmt, rc: integer;
begin
  db := SqliteOpen(':memory:');
  rc := SqliteExec(db, 'CREATE TABLE t(id INTEGER, name TEXT, score REAL)');
  stmt := SqlitePrepare(db, 'INSERT INTO t VALUES (?, ?, ?)');
  rc := SqliteBindInt(stmt, 1, 1);
  rc := SqliteBindText(stmt, 2, 'alice');
  rc := SqliteBindDouble(stmt, 3, 91.5);
  rc := SqliteStep(stmt);
  rc := SqliteReset(stmt);
  rc := SqliteClearBindings(stmt);
  rc := SqliteBindInt(stmt, 1, 2);
  rc := SqliteBindText(stmt, 2, 'bob');
  rc := SqliteBindNull(stmt, 3);
  rc := SqliteStep(stmt);
  rc := SqliteFinalize(stmt);

  writeln('changes: ', SqliteChanges(db));
  writeln('last rowid: ', SqliteLastInsertRowId(db));

  stmt := SqlitePrepare(db, 'SELECT id, name, score FROM t ORDER BY id');
  writeln('columns: ', SqliteColumnCount(stmt));
  while SqliteStep(stmt) = 100 do
  begin
    writeln(SqliteColumnInt(stmt, 0), ' ',
             SqliteColumnName(stmt, 1), '=', SqliteColumnText(stmt, 1), ' (', SqliteColumnType(stmt, 1), ') ',
             SqliteColumnName(stmt, 2), '=', SqliteColumnDouble(stmt, 2), ' (', SqliteColumnType(stmt, 2), ')');
  end;
  rc := SqliteFinalize(stmt);
  rc := SqliteClose(db);
  writeln('done');
end.
