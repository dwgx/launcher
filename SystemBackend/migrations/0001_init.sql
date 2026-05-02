-- Launcher 后端初始 schema。
-- 字段命名：snake_case；时间戳全部 timestamptz；金额/计数全部 BIGINT。

CREATE EXTENSION IF NOT EXISTS pgcrypto;

CREATE TABLE IF NOT EXISTS users (
    id                       UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    username_hash            TEXT NOT NULL UNIQUE,
    password_hash            TEXT NOT NULL,        -- argon2id PHC
    hwid_bound               TEXT,                  -- 服务端二次盐化后的 HWID
    subscription_tier        TEXT,                  -- 1day/3day/1week/2week/1month
    subscription_expires_at  TIMESTAMPTZ,
    created_at               TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_login_at            TIMESTAMPTZ,
    note                     TEXT
);

CREATE TABLE IF NOT EXISTS sessions (
    token       TEXT PRIMARY KEY,
    user_id     UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    expires_at  TIMESTAMPTZ NOT NULL,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    user_agent  TEXT,
    remote_ip   INET
);
CREATE INDEX IF NOT EXISTS idx_sessions_user_id ON sessions(user_id);
CREATE INDEX IF NOT EXISTS idx_sessions_expires ON sessions(expires_at);

CREATE TABLE IF NOT EXISTS subscriptions (
    id          TEXT PRIMARY KEY,
    user_id     UUID REFERENCES users(id) ON DELETE CASCADE,
    name        TEXT NOT NULL,
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at  TIMESTAMPTZ,
    helix_blob  BYTEA,                              -- 可选：DB 内副本，CDN 不可达时回退
    issuer      TEXT
);
CREATE INDEX IF NOT EXISTS idx_subscriptions_user_id ON subscriptions(user_id);

CREATE TABLE IF NOT EXISTS audit_log (
    id          BIGSERIAL PRIMARY KEY,
    actor       TEXT,
    action      TEXT NOT NULL,
    target      TEXT,
    metadata    JSONB,
    occurred_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_audit_occurred_at ON audit_log(occurred_at DESC);

-- 客户端心跳告警：每分钟一行/每用户，用于检测离线/异常
CREATE TABLE IF NOT EXISTS heartbeats (
    user_id    UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    received_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    client_ver  TEXT,
    PRIMARY KEY (user_id, received_at)
);
