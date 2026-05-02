#pragma once

// SQLite 缓存层：仅放非敏感数据（订阅元数据缓存、游戏列表、UI 偏好）。
// 敏感数据走 storage::Registry。

#include "app/common.h"

struct sqlite3;

namespace launcher::storage {

class SqliteStore {
public:
    SqliteStore();
    ~SqliteStore();
    LAUNCHER_DISALLOW_COPY(SqliteStore);

    Status open(const std::string& path);
    void   close();

    Status execute(const std::string& sql);

    sqlite3* raw() { return m_db; }

private:
    sqlite3* m_db{nullptr};
};

}  // namespace launcher::storage
