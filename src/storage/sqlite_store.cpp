#include "storage/sqlite_store.h"

#include <sqlite3.h>
#include <spdlog/spdlog.h>

namespace launcher::storage {

SqliteStore::SqliteStore() = default;
SqliteStore::~SqliteStore() { close(); }

Status SqliteStore::open(const std::string& path) {
    int rc = sqlite3_open_v2(path.c_str(), &m_db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (rc != SQLITE_OK) {
        spdlog::error("sqlite3_open_v2 failed: {}", sqlite3_errmsg(m_db));
        return Status::Err(rc);
    }
    sqlite3_exec(m_db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(m_db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(m_db, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);

    // bootstrap schema
    const char* kSchema = R"SQL(
        CREATE TABLE IF NOT EXISTS subscription_cache (
            id          TEXT PRIMARY KEY,
            name        TEXT NOT NULL,
            updated_at  INTEGER NOT NULL,
            payload     BLOB    NOT NULL
        );
        CREATE TABLE IF NOT EXISTS game_meta (
            game_id     TEXT PRIMARY KEY,
            sub_id      TEXT NOT NULL,
            display_name TEXT,
            version     TEXT,
            local_hash  BLOB,
            last_played INTEGER
        );
        CREATE TABLE IF NOT EXISTS ui_pref (
            key TEXT PRIMARY KEY,
            value TEXT
        );
    )SQL";
    return execute(kSchema);
}

void SqliteStore::close() {
    if (m_db) { sqlite3_close_v2(m_db); m_db = nullptr; }
}

Status SqliteStore::execute(const std::string& sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        spdlog::error("sqlite exec failed: {}", err ? err : "(null)");
        if (err) sqlite3_free(err);
        return Status::Err(rc);
    }
    return Status::Ok();
}

}  // namespace launcher::storage
