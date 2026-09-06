#pragma once

#include "imagine/db/connection.hpp"
#include "imagine/common/error.hpp"

namespace imagine::db {

class Schema {
public:
    static constexpr int CurrentVersion = 3;

    static Status migrate(Connection& conn);
    static Result<int> getCurrentVersion(Connection& conn);

private:
    static Status applyMigrationV1(Connection& conn);
    static Status applyMigrationV2(Connection& conn);
    static Status applyMigrationV3(Connection& conn);
};

} // namespace imagine::db
