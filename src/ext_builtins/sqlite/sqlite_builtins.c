#include "core/utils.h"
#include "backend_ast/builtin.h"
#include "ext_builtins/registry.h"

#include <sqlite3.h>

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    SQLITE_HANDLE_UNUSED = 0,
    SQLITE_HANDLE_DB,
    SQLITE_HANDLE_STATEMENT
} SqliteHandleKind;

typedef struct {
    SqliteHandleKind kind;
    sqlite3 *db;
    sqlite3_stmt *stmt;
    int db_handle;
} SqliteHandleEntry;

static SqliteHandleEntry *sqliteHandleTable = NULL;
static size_t sqliteHandleCapacity = 0;
static pthread_mutex_t sqliteHandleMutex = PTHREAD_MUTEX_INITIALIZER;

static void sqliteResetEntry(SqliteHandleEntry *entry) {
    if (!entry) {
        return;
    }
    entry->kind = SQLITE_HANDLE_UNUSED;
    entry->db = NULL;
    entry->stmt = NULL;
    entry->db_handle = -1;
}

static size_t sqliteFindFreeSlotLocked(void) {
    for (size_t i = 0; i < sqliteHandleCapacity; ++i) {
        if (sqliteHandleTable[i].kind == SQLITE_HANDLE_UNUSED) {
            return i;
        }
    }
    size_t new_capacity = sqliteHandleCapacity ? sqliteHandleCapacity * 2 : 16;
    SqliteHandleEntry *new_table = (SqliteHandleEntry *)realloc(
        sqliteHandleTable, new_capacity * sizeof(SqliteHandleEntry));
    if (!new_table) {
        return (size_t)-1;
    }
    for (size_t i = sqliteHandleCapacity; i < new_capacity; ++i) {
        sqliteResetEntry(&new_table[i]);
    }
    sqliteHandleTable = new_table;
    size_t slot = sqliteHandleCapacity;
    sqliteHandleCapacity = new_capacity;
    return slot;
}

static int sqliteAllocDbHandleLocked(sqlite3 *db) {
    size_t slot = sqliteFindFreeSlotLocked();
    if (slot == (size_t)-1) {
        return -1;
    }
    sqliteHandleTable[slot].kind = SQLITE_HANDLE_DB;
    sqliteHandleTable[slot].db = db;
    sqliteHandleTable[slot].stmt = NULL;
    sqliteHandleTable[slot].db_handle = -1;
    return (int)slot;
}

static int sqliteAllocStmtHandleLocked(sqlite3_stmt *stmt, int db_handle) {
    size_t slot = sqliteFindFreeSlotLocked();
    if (slot == (size_t)-1) {
        return -1;
    }
    sqliteHandleTable[slot].kind = SQLITE_HANDLE_STATEMENT;
    sqliteHandleTable[slot].stmt = stmt;
    sqliteHandleTable[slot].db = NULL;
    sqliteHandleTable[slot].db_handle = db_handle;
    return (int)slot;
}

static SqliteHandleEntry *sqliteLookupHandleLocked(int handle) {
    if (handle < 0) {
        return NULL;
    }
    size_t idx = (size_t)handle;
    if (idx >= sqliteHandleCapacity) {
        return NULL;
    }
    return &sqliteHandleTable[idx];
}

static const char *sqliteTypeToString(int type) {
    switch (type) {
        case SQLITE_INTEGER: return "INTEGER";
        case SQLITE_FLOAT:   return "FLOAT";
        case SQLITE_TEXT:    return "TEXT";
        case SQLITE_BLOB:    return "BLOB";
        case SQLITE_NULL:    return "NULL";
        default:             return "UNKNOWN";
    }
}

static Value vmSqliteOpen(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1) {
        runtimeError(vm, "SqliteOpen expects exactly 1 argument.");
        return makeInt(-1);
    }
    if (args[0].type != TYPE_STRING) {
        runtimeError(vm, "SqliteOpen argument must be a string path.");
        return makeInt(-1);
    }
    const char *path = args[0].s_val ? args[0].s_val : ":memory:";
    sqlite3 *db = NULL;
    int rc = sqlite3_open(path, &db);
    if (rc != SQLITE_OK) {
        const char *msg = db ? sqlite3_errmsg(db) : "sqlite3_open failed";
        runtimeError(vm, "SqliteOpen failed (%d): %s", rc, msg ? msg : "unknown");
        if (db) {
            sqlite3_close(db);
        }
        return makeInt(-1);
    }

    pthread_mutex_lock(&sqliteHandleMutex);
    int handle = sqliteAllocDbHandleLocked(db);
    pthread_mutex_unlock(&sqliteHandleMutex);

    if (handle < 0) {
        runtimeError(vm, "SqliteOpen: unable to allocate handle.");
        sqlite3_close(db);
        return makeInt(-1);
    }
    return makeInt(handle);
}

static Value vmSqliteClose(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1) {
        runtimeError(vm, "SqliteClose expects exactly 1 argument.");
        return makeInt(-1);
    }
    if (!IS_INTLIKE(args[0])) {
        runtimeError(vm, "SqliteClose argument must be an integer handle.");
        return makeInt(-1);
    }
    int handle = (int)asI64(args[0]);
    sqlite3 *db = NULL;

    pthread_mutex_lock(&sqliteHandleMutex);
    SqliteHandleEntry *entry = sqliteLookupHandleLocked(handle);
    if (!entry || entry->kind != SQLITE_HANDLE_DB || !entry->db) {
        pthread_mutex_unlock(&sqliteHandleMutex);
        runtimeError(vm, "SqliteClose received invalid database handle %d.", handle);
        return makeInt(-1);
    }
    db = entry->db;
    sqliteResetEntry(entry);

    for (size_t i = 0; i < sqliteHandleCapacity; ++i) {
        SqliteHandleEntry *stmt_entry = &sqliteHandleTable[i];
        if (stmt_entry->kind == SQLITE_HANDLE_STATEMENT && stmt_entry->db_handle == handle) {
            sqlite3_stmt *stmt = stmt_entry->stmt;
            sqliteResetEntry(stmt_entry);
            if (stmt) {
                pthread_mutex_unlock(&sqliteHandleMutex);
                int finalize_rc = sqlite3_finalize(stmt);
                if (finalize_rc != SQLITE_OK) {
                    runtimeError(vm, "SqliteClose: sqlite3_finalize returned %d.", finalize_rc);
                }
                pthread_mutex_lock(&sqliteHandleMutex);
            }
        }
    }
    pthread_mutex_unlock(&sqliteHandleMutex);

    if (!db) {
        runtimeError(vm, "SqliteClose: database pointer already cleared.");
        return makeInt(-1);
    }

    int rc = sqlite3_close(db);
    if (rc != SQLITE_OK) {
        runtimeError(vm, "SqliteClose failed (%d): %s", rc, sqlite3_errmsg(db));
    }
    return makeInt(rc);
}

static Value vmSqliteExec(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 2) {
        runtimeError(vm, "SqliteExec expects (db_handle:int, sql:string).");
        return makeInt(-1);
    }
    if (!IS_INTLIKE(args[0]) || args[1].type != TYPE_STRING) {
        runtimeError(vm, "SqliteExec argument types are (int, string).");
        return makeInt(-1);
    }
    int handle = (int)asI64(args[0]);
    const char *sql = args[1].s_val;
    if (!sql) {
        runtimeError(vm, "SqliteExec received NIL SQL string.");
        return makeInt(-1);
    }

    pthread_mutex_lock(&sqliteHandleMutex);
    SqliteHandleEntry *entry = sqliteLookupHandleLocked(handle);
    if (!entry || entry->kind != SQLITE_HANDLE_DB || !entry->db) {
        pthread_mutex_unlock(&sqliteHandleMutex);
        runtimeError(vm, "SqliteExec received invalid database handle %d.", handle);
        return makeInt(-1);
    }
    sqlite3 *db = entry->db;
    pthread_mutex_unlock(&sqliteHandleMutex);

    char *err_msg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        runtimeError(vm, "SqliteExec failed (%d): %s", rc, err_msg ? err_msg : "unknown");
    }
    if (err_msg) {
        sqlite3_free(err_msg);
    }
    return makeInt(rc);
}

static Value vmSqlitePrepare(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 2) {
        runtimeError(vm, "SqlitePrepare expects (db_handle:int, sql:string).");
        return makeInt(-1);
    }
    if (!IS_INTLIKE(args[0]) || args[1].type != TYPE_STRING) {
        runtimeError(vm, "SqlitePrepare argument types are (int, string).");
        return makeInt(-1);
    }
    int handle = (int)asI64(args[0]);
    const char *sql = args[1].s_val;
    if (!sql) {
        runtimeError(vm, "SqlitePrepare received NIL SQL string.");
        return makeInt(-1);
    }

    pthread_mutex_lock(&sqliteHandleMutex);
    SqliteHandleEntry *entry = sqliteLookupHandleLocked(handle);
    if (!entry || entry->kind != SQLITE_HANDLE_DB || !entry->db) {
        pthread_mutex_unlock(&sqliteHandleMutex);
        runtimeError(vm, "SqlitePrepare received invalid database handle %d.", handle);
        return makeInt(-1);
    }
    sqlite3 *db = entry->db;
    int result_handle = -1;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK && stmt) {
        result_handle = sqliteAllocStmtHandleLocked(stmt, handle);
        if (result_handle < 0) {
            sqlite3_finalize(stmt);
            runtimeError(vm, "SqlitePrepare: unable to allocate statement handle.");
        }
    } else {
        runtimeError(vm, "SqlitePrepare failed (%d): %s", rc, sqlite3_errmsg(db));
    }
    pthread_mutex_unlock(&sqliteHandleMutex);

    if (result_handle < 0) {
        return makeInt(-1);
    }
    return makeInt(result_handle);
}

static Value vmSqliteFinalize(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1) {
        runtimeError(vm, "SqliteFinalize expects exactly 1 argument.");
        return makeInt(-1);
    }
    if (!IS_INTLIKE(args[0])) {
        runtimeError(vm, "SqliteFinalize argument must be an integer handle.");
        return makeInt(-1);
    }
    int handle = (int)asI64(args[0]);

    sqlite3_stmt *stmt = NULL;
    pthread_mutex_lock(&sqliteHandleMutex);
    SqliteHandleEntry *entry = sqliteLookupHandleLocked(handle);
    if (!entry || entry->kind != SQLITE_HANDLE_STATEMENT || !entry->stmt) {
        pthread_mutex_unlock(&sqliteHandleMutex);
        runtimeError(vm, "SqliteFinalize received invalid statement handle %d.", handle);
        return makeInt(-1);
    }
    stmt = entry->stmt;
    sqliteResetEntry(entry);
    pthread_mutex_unlock(&sqliteHandleMutex);

    int rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_OK) {
        runtimeError(vm, "SqliteFinalize failed (%d).", rc);
    }
    return makeInt(rc);
}

static bool sqliteFetchStatement(struct VM_s *vm, Value *args, int count, int *out_handle, sqlite3_stmt **out_stmt) {
    if (count < 1) {
        runtimeError(vm, "SQLite statement operation missing handle argument.");
        return false;
    }
    if (!IS_INTLIKE(args[0])) {
        runtimeError(vm, "SQLite statement handle must be an integer.");
        return false;
    }
    int handle = (int)asI64(args[0]);
    pthread_mutex_lock(&sqliteHandleMutex);
    SqliteHandleEntry *entry = sqliteLookupHandleLocked(handle);
    if (!entry || entry->kind != SQLITE_HANDLE_STATEMENT || !entry->stmt) {
        pthread_mutex_unlock(&sqliteHandleMutex);
        runtimeError(vm, "SQLite operation received invalid statement handle %d.", handle);
        return false;
    }
    sqlite3_stmt *stmt = entry->stmt;
    pthread_mutex_unlock(&sqliteHandleMutex);
    if (out_handle) {
        *out_handle = handle;
    }
    if (out_stmt) {
        *out_stmt = stmt;
    }
    return true;
}

static bool sqliteFetchDatabase(struct VM_s *vm, Value *args, int count, int *out_handle, sqlite3 **out_db) {
    if (count < 1) {
        runtimeError(vm, "SQLite database operation missing handle argument.");
        return false;
    }
    if (!IS_INTLIKE(args[0])) {
        runtimeError(vm, "SQLite database handle must be an integer.");
        return false;
    }
    int handle = (int)asI64(args[0]);
    pthread_mutex_lock(&sqliteHandleMutex);
    SqliteHandleEntry *entry = sqliteLookupHandleLocked(handle);
    if (!entry || entry->kind != SQLITE_HANDLE_DB || !entry->db) {
        pthread_mutex_unlock(&sqliteHandleMutex);
        runtimeError(vm, "SQLite operation received invalid database handle %d.", handle);
        return false;
    }
    sqlite3 *db = entry->db;
    pthread_mutex_unlock(&sqliteHandleMutex);
    if (out_handle) {
        *out_handle = handle;
    }
    if (out_db) {
        *out_db = db;
    }
    return true;
}

static Value vmSqliteStep(struct VM_s *vm, int arg_count, Value *args) {
    sqlite3_stmt *stmt = NULL;
    if (!sqliteFetchStatement(vm, args, arg_count, NULL, &stmt)) {
        return makeInt(-1);
    }
    int rc = sqlite3_step(stmt);
    return makeInt(rc);
}

static Value vmSqliteReset(struct VM_s *vm, int arg_count, Value *args) {
    sqlite3_stmt *stmt = NULL;
    if (!sqliteFetchStatement(vm, args, arg_count, NULL, &stmt)) {
        return makeInt(-1);
    }
    int rc = sqlite3_reset(stmt);
    return makeInt(rc);
}

static bool sqliteValidateColumnIndex(struct VM_s *vm, sqlite3_stmt *stmt, int column) {
    if (column < 0 || column >= sqlite3_column_count(stmt)) {
        runtimeError(vm, "SQLite column index %d out of range.", column);
        return false;
    }
    return true;
}

static Value vmSqliteColumnCount(struct VM_s *vm, int arg_count, Value *args) {
    sqlite3_stmt *stmt = NULL;
    if (!sqliteFetchStatement(vm, args, arg_count, NULL, &stmt)) {
        return makeInt(-1);
    }
    int count = sqlite3_column_count(stmt);
    return makeInt(count);
}

static Value vmSqliteColumnType(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 2) {
        runtimeError(vm, "SqliteColumnType expects (stmt_handle:int, column:int).");
        return makeString("");
    }
    sqlite3_stmt *stmt = NULL;
    if (!sqliteFetchStatement(vm, args, arg_count, NULL, &stmt)) {
        return makeString("");
    }
    if (!IS_INTLIKE(args[1])) {
        runtimeError(vm, "SqliteColumnType column index must be integer.");
        return makeString("");
    }
    int column = (int)asI64(args[1]);
    if (!sqliteValidateColumnIndex(vm, stmt, column)) {
        return makeString("");
    }
    int type = sqlite3_column_type(stmt, column);
    return makeString(sqliteTypeToString(type));
}

static Value vmSqliteColumnName(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 2) {
        runtimeError(vm, "SqliteColumnName expects (stmt_handle:int, column:int).");
        return makeString("");
    }
    sqlite3_stmt *stmt = NULL;
    if (!sqliteFetchStatement(vm, args, arg_count, NULL, &stmt)) {
        return makeString("");
    }
    if (!IS_INTLIKE(args[1])) {
        runtimeError(vm, "SqliteColumnName column index must be integer.");
        return makeString("");
    }
    int column = (int)asI64(args[1]);
    if (!sqliteValidateColumnIndex(vm, stmt, column)) {
        return makeString("");
    }
    const char *name = sqlite3_column_name(stmt, column);
    return makeString(name ? name : "");
}

static Value vmSqliteColumnInt(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 2) {
        runtimeError(vm, "SqliteColumnInt expects (stmt_handle:int, column:int).");
        return makeInt64(0);
    }
    sqlite3_stmt *stmt = NULL;
    if (!sqliteFetchStatement(vm, args, arg_count, NULL, &stmt)) {
        return makeInt64(0);
    }
    if (!IS_INTLIKE(args[1])) {
        runtimeError(vm, "SqliteColumnInt column index must be integer.");
        return makeInt64(0);
    }
    int column = (int)asI64(args[1]);
    if (!sqliteValidateColumnIndex(vm, stmt, column)) {
        return makeInt64(0);
    }
    sqlite3_int64 value = sqlite3_column_int64(stmt, column);
    return makeInt64((long long)value);
}

static Value vmSqliteColumnDouble(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 2) {
        runtimeError(vm, "SqliteColumnDouble expects (stmt_handle:int, column:int).");
        return makeDouble(0.0);
    }
    sqlite3_stmt *stmt = NULL;
    if (!sqliteFetchStatement(vm, args, arg_count, NULL, &stmt)) {
        return makeDouble(0.0);
    }
    if (!IS_INTLIKE(args[1])) {
        runtimeError(vm, "SqliteColumnDouble column index must be integer.");
        return makeDouble(0.0);
    }
    int column = (int)asI64(args[1]);
    if (!sqliteValidateColumnIndex(vm, stmt, column)) {
        return makeDouble(0.0);
    }
    double value = sqlite3_column_double(stmt, column);
    return makeDouble(value);
}

static Value vmSqliteColumnText(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 2) {
        runtimeError(vm, "SqliteColumnText expects (stmt_handle:int, column:int).");
        return makeString("");
    }
    sqlite3_stmt *stmt = NULL;
    if (!sqliteFetchStatement(vm, args, arg_count, NULL, &stmt)) {
        return makeString("");
    }
    if (!IS_INTLIKE(args[1])) {
        runtimeError(vm, "SqliteColumnText column index must be integer.");
        return makeString("");
    }
    int column = (int)asI64(args[1]);
    if (!sqliteValidateColumnIndex(vm, stmt, column)) {
        return makeString("");
    }
    const unsigned char *text = sqlite3_column_text(stmt, column);
    int len = sqlite3_column_bytes(stmt, column);
    if (!text || len <= 0) {
        return makeString("");
    }
    return makeStringLen((const char *)text, (size_t)len);
}

static Value vmSqliteBindText(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 3) {
        runtimeError(vm, "SqliteBindText expects (stmt_handle:int, index:int, value:string).");
        return makeInt(-1);
    }
    sqlite3_stmt *stmt = NULL;
    if (!sqliteFetchStatement(vm, args, arg_count, NULL, &stmt)) {
        return makeInt(-1);
    }
    if (!IS_INTLIKE(args[1])) {
        runtimeError(vm, "SqliteBindText index must be integer.");
        return makeInt(-1);
    }
    if (args[2].type != TYPE_STRING) {
        runtimeError(vm, "SqliteBindText value must be string.");
        return makeInt(-1);
    }
    int index = (int)asI64(args[1]);
    if (index <= 0) {
        runtimeError(vm, "SqliteBindText parameter index must be >= 1.");
        return makeInt(-1);
    }
    const char *text = args[2].s_val ? args[2].s_val : "";
    int rc = sqlite3_bind_text(stmt, index, text, -1, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        runtimeError(vm, "SqliteBindText failed (%d).", rc);
    }
    return makeInt(rc);
}

static Value vmSqliteBindInt(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 3) {
        runtimeError(vm, "SqliteBindInt expects (stmt_handle:int, index:int, value:int).");
        return makeInt(-1);
    }
    sqlite3_stmt *stmt = NULL;
    if (!sqliteFetchStatement(vm, args, arg_count, NULL, &stmt)) {
        return makeInt(-1);
    }
    if (!IS_INTLIKE(args[1]) || !IS_INTLIKE(args[2])) {
        runtimeError(vm, "SqliteBindInt arguments must be integers.");
        return makeInt(-1);
    }
    int index = (int)asI64(args[1]);
    if (index <= 0) {
        runtimeError(vm, "SqliteBindInt parameter index must be >= 1.");
        return makeInt(-1);
    }
    sqlite3_int64 value = (sqlite3_int64)asI64(args[2]);
    int rc = sqlite3_bind_int64(stmt, index, value);
    if (rc != SQLITE_OK) {
        runtimeError(vm, "SqliteBindInt failed (%d).", rc);
    }
    return makeInt(rc);
}

static Value vmSqliteBindDouble(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 3) {
        runtimeError(vm, "SqliteBindDouble expects (stmt_handle:int, index:int, value:real).");
        return makeInt(-1);
    }
    sqlite3_stmt *stmt = NULL;
    if (!sqliteFetchStatement(vm, args, arg_count, NULL, &stmt)) {
        return makeInt(-1);
    }
    if (!IS_INTLIKE(args[1])) {
        runtimeError(vm, "SqliteBindDouble index must be integer.");
        return makeInt(-1);
    }
    int index = (int)asI64(args[1]);
    if (index <= 0) {
        runtimeError(vm, "SqliteBindDouble parameter index must be >= 1.");
        return makeInt(-1);
    }
    double value;
    if (IS_REAL(args[2])) {
        value = AS_REAL(args[2]);
    } else if (IS_INTLIKE(args[2])) {
        value = (double)asI64(args[2]);
    } else {
        runtimeError(vm, "SqliteBindDouble value must be numeric.");
        return makeInt(-1);
    }
    int rc = sqlite3_bind_double(stmt, index, value);
    if (rc != SQLITE_OK) {
        runtimeError(vm, "SqliteBindDouble failed (%d).", rc);
    }
    return makeInt(rc);
}

static Value vmSqliteBindNull(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 2) {
        runtimeError(vm, "SqliteBindNull expects (stmt_handle:int, index:int).");
        return makeInt(-1);
    }
    sqlite3_stmt *stmt = NULL;
    if (!sqliteFetchStatement(vm, args, arg_count, NULL, &stmt)) {
        return makeInt(-1);
    }
    if (!IS_INTLIKE(args[1])) {
        runtimeError(vm, "SqliteBindNull index must be integer.");
        return makeInt(-1);
    }
    int index = (int)asI64(args[1]);
    if (index <= 0) {
        runtimeError(vm, "SqliteBindNull parameter index must be >= 1.");
        return makeInt(-1);
    }
    int rc = sqlite3_bind_null(stmt, index);
    if (rc != SQLITE_OK) {
        runtimeError(vm, "SqliteBindNull failed (%d).", rc);
    }
    return makeInt(rc);
}

static Value vmSqliteClearBindings(struct VM_s *vm, int arg_count, Value *args) {
    sqlite3_stmt *stmt = NULL;
    if (!sqliteFetchStatement(vm, args, arg_count, NULL, &stmt)) {
        return makeInt(-1);
    }
    int rc = sqlite3_clear_bindings(stmt);
    if (rc != SQLITE_OK) {
        runtimeError(vm, "SqliteClearBindings failed (%d).", rc);
    }
    return makeInt(rc);
}

static Value vmSqliteErrMsg(struct VM_s *vm, int arg_count, Value *args) {
    sqlite3 *db = NULL;
    if (!sqliteFetchDatabase(vm, args, arg_count, NULL, &db)) {
        return makeString("");
    }
    const char *msg = sqlite3_errmsg(db);
    return makeString(msg ? msg : "");
}

static Value vmSqliteLastInsertRowId(struct VM_s *vm, int arg_count, Value *args) {
    sqlite3 *db = NULL;
    if (!sqliteFetchDatabase(vm, args, arg_count, NULL, &db)) {
        return makeInt64(0);
    }
    sqlite3_int64 rowid = sqlite3_last_insert_rowid(db);
    return makeInt64((long long)rowid);
}

static Value vmSqliteChanges(struct VM_s *vm, int arg_count, Value *args) {
    sqlite3 *db = NULL;
    if (!sqliteFetchDatabase(vm, args, arg_count, NULL, &db)) {
        return makeInt(0);
    }
    int changes = sqlite3_changes(db);
    return makeInt(changes);
}

static void registerSqliteFunction(const char *display_name,
                                   const char *vm_name,
                                   VmBuiltinFn fn) {
    extBuiltinRegisterFunction("sqlite", display_name);
    registerVmBuiltin(vm_name, fn, BUILTIN_TYPE_FUNCTION, display_name);
}

void registerSqliteBuiltins(void) {
    extBuiltinRegisterCategory("sqlite");

    registerSqliteFunction("SqliteOpen", "sqliteopen", vmSqliteOpen);
    registerSqliteFunction("SqliteClose", "sqliteclose", vmSqliteClose);
    registerSqliteFunction("SqliteExec", "sqliteexec", vmSqliteExec);
    registerSqliteFunction("SqlitePrepare", "sqliteprepare", vmSqlitePrepare);
    registerSqliteFunction("SqliteFinalize", "sqlitefinalize", vmSqliteFinalize);
    registerSqliteFunction("SqliteStep", "sqlitestep", vmSqliteStep);
    registerSqliteFunction("SqliteReset", "sqlitereset", vmSqliteReset);
    registerSqliteFunction("SqliteColumnCount", "sqlitecolumncount", vmSqliteColumnCount);
    registerSqliteFunction("SqliteColumnType", "sqlitecolumntype", vmSqliteColumnType);
    registerSqliteFunction("SqliteColumnName", "sqlitecolumnname", vmSqliteColumnName);
    registerSqliteFunction("SqliteColumnInt", "sqlitecolumnint", vmSqliteColumnInt);
    registerSqliteFunction("SqliteColumnDouble", "sqlitecolumndouble", vmSqliteColumnDouble);
    registerSqliteFunction("SqliteColumnText", "sqlitecolumntext", vmSqliteColumnText);
    registerSqliteFunction("SqliteBindText", "sqlitebindtext", vmSqliteBindText);
    registerSqliteFunction("SqliteBindInt", "sqlitebindint", vmSqliteBindInt);
    registerSqliteFunction("SqliteBindDouble", "sqlitebinddouble", vmSqliteBindDouble);
    registerSqliteFunction("SqliteBindNull", "sqlitebindnull", vmSqliteBindNull);
    registerSqliteFunction("SqliteClearBindings", "sqliteclearbindings", vmSqliteClearBindings);
    registerSqliteFunction("SqliteErrMsg", "sqliteerrmsg", vmSqliteErrMsg);
    registerSqliteFunction("SqliteLastInsertRowId", "sqlitelastinsertrowid", vmSqliteLastInsertRowId);
    registerSqliteFunction("SqliteChanges", "sqlitechanges", vmSqliteChanges);
}
