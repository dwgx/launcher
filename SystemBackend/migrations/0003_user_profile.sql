-- 用户三段身份模型 + 历史登录 + 修改频率限制 + 头像存储。
--
-- UID:       注册时随机生成（例如 base32 8 位 "K8RX2QZP"），公开显示给其他用户，**不可改**
-- Username:  注册时用户输入，登录用，**不可改**（admin 例外）
-- Nickname:  用户可自改，受频率限制（默认 7 天一次），allowed list / blocklist 可后续扩展
--
-- avatar 文件存在 server 文件系统（/opt/systembackend/avatars/<user_id>.<ext>）
-- 不进数据库（避免 BLOB 膨胀），只存路径 + mime + size。

ALTER TABLE users
    ADD COLUMN IF NOT EXISTS uid                  TEXT UNIQUE,                  -- 公开短 ID
    ADD COLUMN IF NOT EXISTS username             TEXT UNIQUE,                  -- 登录用
    ADD COLUMN IF NOT EXISTS nickname             TEXT,                          -- 显示名
    ADD COLUMN IF NOT EXISTS nickname_changed_at  TIMESTAMPTZ,
    ADD COLUMN IF NOT EXISTS avatar_path          TEXT,                          -- /avatars/<id>.<ext>
    ADD COLUMN IF NOT EXISTS avatar_mime          TEXT,                          -- "image/png" / "image/jpeg" / "image/gif"
    ADD COLUMN IF NOT EXISTS avatar_updated_at    TIMESTAMPTZ,
    ADD COLUMN IF NOT EXISTS password_changed_at  TIMESTAMPTZ;

-- 限制频率配置（admin 可后台改，默认 7 天）
CREATE TABLE IF NOT EXISTS rate_limits (
    key            TEXT PRIMARY KEY,
    seconds        INTEGER NOT NULL
);
INSERT INTO rate_limits (key, seconds) VALUES
    ('nickname_change', 7 * 24 * 3600),
    ('avatar_upload',   24 * 3600),
    ('password_change', 60)
ON CONFLICT DO NOTHING;

-- 历史登录：客户端端 IP + UA + HWID hash + 登录方式 + 是否成功
CREATE TABLE IF NOT EXISTS login_history (
    id             BIGSERIAL PRIMARY KEY,
    user_id        UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    occurred_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    success        BOOLEAN NOT NULL,
    failure_reason TEXT,
    remote_ip      INET,
    user_agent     TEXT,
    hwid_short     TEXT,                             -- HWID hex 前 16 位
    client_ver     TEXT,
    geo_country    TEXT,
    geo_city       TEXT
);
CREATE INDEX IF NOT EXISTS idx_login_history_user_time
    ON login_history(user_id, occurred_at DESC);

-- 头像版本号：方便客户端缓存破坏 (avatar?v=3)
CREATE TABLE IF NOT EXISTS user_avatar_meta (
    user_id       UUID PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
    version       INTEGER NOT NULL DEFAULT 1,
    bytes         INTEGER NOT NULL DEFAULT 0,
    updated_at    TIMESTAMPTZ NOT NULL DEFAULT now()
);
