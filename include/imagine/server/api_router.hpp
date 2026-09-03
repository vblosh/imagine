#pragma once

#include <string>
#include <memory>
#include "imagine/common/types.hpp"
#include "imagine/common/error.hpp"
#include "imagine/db/catalog_db.hpp"
#include "imagine/thumbnail/cache.hpp"

namespace httplib {
class Server;
struct Request;
struct Response;
}

namespace imagine::core {
class Catalog;
}

namespace imagine::server {

class ApiRouter {
public:
    explicit ApiRouter(core::Catalog& catalog);
    ApiRouter(db::CatalogDb& db, thumbnail::Cache& cache);
    ~ApiRouter();

    void registerRoutes(httplib::Server& server);

    core::Catalog* catalog() const { return catalog_; }
    db::CatalogDb& db() const;
    thumbnail::Cache& cache() const;

private:
    void registerCorsHandler(httplib::Server& server);
    void registerMediaRoutes(httplib::Server& server);
    void registerTagRoutes(httplib::Server& server);
    void registerAlbumRoutes(httplib::Server& server);
    void registerThumbnailRoutes(httplib::Server& server);
    void registerTimelineRoutes(httplib::Server& server);
    void registerStatsRoutes(httplib::Server& server);
    void registerImportRoutes(httplib::Server& server);

    core::Catalog* catalog_{nullptr};
    db::CatalogDb* db_{nullptr};
    thumbnail::Cache* cache_{nullptr};
};

} // namespace imagine::server
