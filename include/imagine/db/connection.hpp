#pragma once

#include <string>
#include <string_view>
#include <memory>
#include <optional>
#include <cstdint>
#include <sqlite3.h>
#include "imagine/common/error.hpp"

namespace imagine::db {

enum class StepResult {
    Row,
    Done,
    Error
};

class Connection;

class Statement {
public:
    Statement() = default;
    Statement(sqlite3_stmt* stmt);
    ~Statement();

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;

    bool isValid() const noexcept { return stmt_ != nullptr; }

    Status bind(int index, int32_t val);
    Status bind(int index, int64_t val);
    Status bind(int index, double val);
    Status bind(int index, const std::string& val);
    Status bindNull(int index);

    template <typename T>
    Status bindOptional(int index, const std::optional<T>& opt) {
        if (opt.has_value()) {
            return bind(index, *opt);
        } else {
            return bindNull(index);
        }
    }

    StepResult step();
    Status reset();

    bool isNull(int col) const;
    int32_t getInt(int col) const;
    int64_t getInt64(int col) const;
    double getDouble(int col) const;
    std::string getString(int col) const;
    std::optional<std::string> getOptionalString(int col) const;

    sqlite3_stmt* raw() const { return stmt_; }
    int columnCount() const noexcept { return stmt_ ? sqlite3_column_count(stmt_) : 0; }

private:
    sqlite3_stmt* stmt_{nullptr};
};

class Connection {
public:
    Connection() = default;
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;

    Status open(const std::string& dbPath);
    void close();
    bool isOpen() const noexcept { return db_ != nullptr; }

    Status execute(const std::string& sql);
    Result<Statement> prepare(const std::string& sql);

    int64_t lastInsertRowId() const;
    int changes() const;
    std::string lastErrorMessage() const;
    int lastErrorCode() const;

    sqlite3* raw() const { return db_; }

private:
    sqlite3* db_{nullptr};
};

class Transaction {
public:
    explicit Transaction(Connection& conn);
    ~Transaction();

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    Status commit();
    Status rollback();

private:
    Connection& conn_;
    bool active_{false};
};

} // namespace imagine::db
