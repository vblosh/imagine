#include "imagine/db/connection.hpp"
#include <filesystem>

namespace imagine::db {

// --- Statement ---

Statement::Statement(sqlite3_stmt* stmt) : stmt_(stmt) {}

Statement::~Statement() {
    if (stmt_) {
        sqlite3_finalize(stmt_);
        stmt_ = nullptr;
    }
}

Statement::Statement(Statement&& other) noexcept : stmt_(other.stmt_) {
    other.stmt_ = nullptr;
}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        if (stmt_) {
            sqlite3_finalize(stmt_);
        }
        stmt_ = other.stmt_;
        other.stmt_ = nullptr;
    }
    return *this;
}

Status Statement::bind(int index, int32_t val) {
    if (!stmt_) return Status::databaseError("Invalid statement handle");
    int rc = sqlite3_bind_int(stmt_, index, val);
    if (rc != SQLITE_OK) {
        return Status::databaseError("Failed to bind int: " + std::string(sqlite3_errstr(rc)));
    }
    return Status::ok();
}

Status Statement::bind(int index, int64_t val) {
    if (!stmt_) return Status::databaseError("Invalid statement handle");
    int rc = sqlite3_bind_int64(stmt_, index, val);
    if (rc != SQLITE_OK) {
        return Status::databaseError("Failed to bind int64: " + std::string(sqlite3_errstr(rc)));
    }
    return Status::ok();
}

Status Statement::bind(int index, double val) {
    if (!stmt_) return Status::databaseError("Invalid statement handle");
    int rc = sqlite3_bind_double(stmt_, index, val);
    if (rc != SQLITE_OK) {
        return Status::databaseError("Failed to bind double: " + std::string(sqlite3_errstr(rc)));
    }
    return Status::ok();
}

Status Statement::bind(int index, std::string_view val) {
    if (!stmt_) return Status::databaseError("Invalid statement handle");
    int rc = sqlite3_bind_text(stmt_, index, val.data(), static_cast<int>(val.size()), SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        return Status::databaseError("Failed to bind text: " + std::string(sqlite3_errstr(rc)));
    }
    return Status::ok();
}

Status Statement::bind(int index, const char* val) {
    if (!val) return bindNull(index);
    return bind(index, std::string_view(val));
}

Status Statement::bindBlob(int index, const void* data, size_t size) {
    if (!stmt_) return Status::databaseError("Invalid statement handle");
    int rc = sqlite3_bind_blob(stmt_, index, data, static_cast<int>(size), SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        return Status::databaseError("Failed to bind blob: " + std::string(sqlite3_errstr(rc)));
    }
    return Status::ok();
}

Status Statement::bindNull(int index) {
    if (!stmt_) return Status::databaseError("Invalid statement handle");
    int rc = sqlite3_bind_null(stmt_, index);
    if (rc != SQLITE_OK) {
        return Status::databaseError("Failed to bind null: " + std::string(sqlite3_errstr(rc)));
    }
    return Status::ok();
}

StepResult Statement::step() {
    if (!stmt_) return StepResult::Error;
    int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return StepResult::Row;
    if (rc == SQLITE_DONE) return StepResult::Done;
    return StepResult::Error;
}

Status Statement::reset() {
    if (!stmt_) return Status::databaseError("Invalid statement handle");
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
    return Status::ok();
}

bool Statement::isNull(int col) const {
    if (!stmt_) return true;
    return sqlite3_column_type(stmt_, col) == SQLITE_NULL;
}

int Statement::columnCount() const {
    if (!stmt_) return 0;
    return sqlite3_column_count(stmt_);
}

int32_t Statement::getInt(int col) const {
    if (!stmt_) return 0;
    return sqlite3_column_int(stmt_, col);
}

int64_t Statement::getInt64(int col) const {
    if (!stmt_) return 0;
    return sqlite3_column_int64(stmt_, col);
}

double Statement::getDouble(int col) const {
    if (!stmt_) return 0.0;
    return sqlite3_column_double(stmt_, col);
}

std::string Statement::getString(int col) const {
    if (!stmt_) return "";
    const unsigned char* text = sqlite3_column_text(stmt_, col);
    if (!text) return "";
    int bytes = sqlite3_column_bytes(stmt_, col);
    return std::string(reinterpret_cast<const char*>(text), bytes);
}

const void* Statement::getBlob(int col) const {
    if (!stmt_) return nullptr;
    return sqlite3_column_blob(stmt_, col);
}

size_t Statement::getBlobBytes(int col) const {
    if (!stmt_) return 0;
    return static_cast<size_t>(sqlite3_column_bytes(stmt_, col));
}

std::optional<std::string> Statement::getOptionalString(int col) const {
    if (!stmt_ || isNull(col)) return std::nullopt;
    return getString(col);
}

std::optional<int32_t> Statement::getOptionalInt(int col) const {
    if (!stmt_ || isNull(col)) return std::nullopt;
    return getInt(col);
}

std::optional<int64_t> Statement::getOptionalInt64(int col) const {
    if (!stmt_ || isNull(col)) return std::nullopt;
    return getInt64(col);
}

std::optional<double> Statement::getOptionalDouble(int col) const {
    if (!stmt_ || isNull(col)) return std::nullopt;
    return getDouble(col);
}

// --- Connection ---

Connection::~Connection() {
    close();
}

Connection::Connection(Connection&& other) noexcept : db_(other.db_) {
    other.db_ = nullptr;
}

Connection& Connection::operator=(Connection&& other) noexcept {
    if (this != &other) {
        close();
        db_ = other.db_;
        other.db_ = nullptr;
    }
    return *this;
}

Status Connection::open(const std::string& dbPath) {
    close();

    if (dbPath != ":memory:") {
        std::filesystem::path p(dbPath);
        if (p.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(p.parent_path(), ec);
        }
    }

    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    int rc = sqlite3_open_v2(dbPath.c_str(), &db_, flags, nullptr);
    if (rc != SQLITE_OK) {
        std::string err = db_ ? sqlite3_errmsg(db_) : "Failed to allocate sqlite handle";
        close();
        return Status::databaseError("Failed to open database '" + dbPath + "': " + err);
    }

    // Configure SQLite performance pragmas
    execute("PRAGMA journal_mode = WAL;");
    execute("PRAGMA synchronous = NORMAL;");
    execute("PRAGMA foreign_keys = ON;");
    execute("PRAGMA temp_store = MEMORY;");
    execute("PRAGMA cache_size = -64000;"); // 64MB cache
    execute("PRAGMA busy_timeout = 5000;"); // 5s timeout on lock contention

    return Status::ok();
}

void Connection::close() {
    if (db_) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }
}

Status Connection::execute(const std::string& sql) {
    if (!db_) return Status::databaseError("Database is not open");
    char* errmsg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::string err = errmsg ? errmsg : "Unknown error";
        sqlite3_free(errmsg);
        return Status::databaseError("SQLite exec failed: " + err + " (SQL: " + sql + ")");
    }
    return Status::ok();
}

Result<Statement> Connection::prepare(const std::string& sql) {
    if (!db_) return Status::databaseError("Database is not open");
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), static_cast<int>(sql.size()), &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return Status::databaseError("Failed to prepare statement: " + std::string(sqlite3_errmsg(db_)) + " (SQL: " + sql + ")");
    }
    return Statement(stmt);
}

int64_t Connection::lastInsertRowId() const {
    return db_ ? sqlite3_last_insert_rowid(db_) : 0;
}

int Connection::changes() const {
    return db_ ? sqlite3_changes(db_) : 0;
}

std::string Connection::lastErrorMessage() const {
    return db_ ? sqlite3_errmsg(db_) : "Database closed";
}

int Connection::lastErrorCode() const {
    return db_ ? sqlite3_errcode(db_) : -1;
}

// --- Transaction ---

Transaction::Transaction(Connection& conn) : conn_(conn) {
    if (!conn_.isOpen()) {
        return;
    }
    savepointName_ = "sp_" + std::to_string(conn_.nextSavepointId());
    if (conn_.execute("SAVEPOINT " + savepointName_ + ";").isOk()) {
        active_ = true;
    }
}

Transaction::~Transaction() {
    if (active_) {
        rollback();
    }
}

Status Transaction::commit() {
    if (!active_) return Status::databaseError("No active transaction to commit");
    Status s = conn_.execute("RELEASE " + savepointName_ + ";");
    if (s.isOk()) {
        active_ = false;
    }
    return s;
}

Status Transaction::rollback() {
    if (!active_) return Status::databaseError("No active transaction to rollback");
    Status s1 = conn_.execute("ROLLBACK TO " + savepointName_ + ";");
    Status s2 = conn_.execute("RELEASE " + savepointName_ + ";");
    active_ = false;
    return s1.isOk() ? s2 : s1;
}

} // namespace imagine::db
