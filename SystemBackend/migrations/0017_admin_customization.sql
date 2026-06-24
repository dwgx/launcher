-- Admin customization v1: database-backed operations config, operators, and revisions.

CREATE TABLE IF NOT EXISTS admin_operators (
    id            UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id       UUID UNIQUE REFERENCES users(id) ON DELETE SET NULL,
    username      TEXT NOT NULL UNIQUE,
    display_name  TEXT NOT NULL DEFAULT '',
    operator_role TEXT NOT NULL DEFAULT 'operator'
        CHECK (operator_role IN ('owner', 'admin', 'operator', 'auditor')),
    enabled       BOOLEAN NOT NULL DEFAULT TRUE,
    note          TEXT,
    created_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_login_at TIMESTAMPTZ
);
CREATE INDEX IF NOT EXISTS idx_admin_operators_enabled_role
    ON admin_operators(enabled, operator_role);

CREATE TABLE IF NOT EXISTS app_config_entries (
    key              TEXT PRIMARY KEY
        CHECK (key ~ '^[a-z0-9][a-z0-9_.-]{2,95}$'),
    category         TEXT NOT NULL DEFAULT 'general',
    label            TEXT NOT NULL,
    description      TEXT NOT NULL DEFAULT '',
    value_type       TEXT NOT NULL DEFAULT 'string'
        CHECK (value_type IN ('string', 'number', 'bool', 'json')),
    value            JSONB NOT NULL DEFAULT 'null'::jsonb,
    enabled          BOOLEAN NOT NULL DEFAULT TRUE,
    expose_to_client BOOLEAN NOT NULL DEFAULT FALSE,
    updated_by       TEXT,
    updated_at       TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_app_config_entries_category
    ON app_config_entries(category, key);
CREATE INDEX IF NOT EXISTS idx_app_config_entries_client
    ON app_config_entries(expose_to_client, enabled);

CREATE TABLE IF NOT EXISTS app_config_revisions (
    id                    BIGSERIAL PRIMARY KEY,
    config_key            TEXT NOT NULL REFERENCES app_config_entries(key) ON DELETE CASCADE,
    old_value             JSONB,
    new_value             JSONB NOT NULL,
    old_enabled           BOOLEAN,
    new_enabled           BOOLEAN NOT NULL,
    old_expose_to_client  BOOLEAN,
    new_expose_to_client  BOOLEAN NOT NULL,
    actor                 TEXT NOT NULL,
    note                  TEXT,
    created_at            TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_app_config_revisions_key_created
    ON app_config_revisions(config_key, created_at DESC);

INSERT INTO app_config_entries
    (key, category, label, description, value_type, value, enabled, expose_to_client, updated_by)
VALUES
    ('branding.server_name', 'branding', '服务器名称', '客户端和后台展示的服务器名称', 'string', to_jsonb('Launcher Server'::TEXT), TRUE, TRUE, 'migration'),
    ('features.announcements', 'features', '公告功能', '是否在客户端 bootstrap 中启用公告入口', 'bool', 'true'::jsonb, TRUE, TRUE, 'migration'),
    ('features.tickets', 'features', '工单功能', '是否在客户端 bootstrap 中启用工单入口', 'bool', 'true'::jsonb, TRUE, TRUE, 'migration'),
    ('features.market', 'features', '市场功能', '是否在客户端 bootstrap 中启用市场入口', 'bool', 'true'::jsonb, TRUE, TRUE, 'migration'),
    ('features.stickers', 'features', '表情包功能', '是否在客户端 bootstrap 中启用表情包入口', 'bool', 'true'::jsonb, TRUE, TRUE, 'migration'),
    ('limits.chat_slowmode_default_seconds', 'limits', '默认慢速发言秒数', '新频道未单独配置时可参考的慢速发言秒数', 'number', '0'::jsonb, TRUE, FALSE, 'migration'),
    ('community.default_area', 'community', '默认社区区块', '新社区频道或入口的默认区块 slug', 'string', to_jsonb('general'::TEXT), TRUE, FALSE, 'migration')
ON CONFLICT (key) DO NOTHING;

DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'helix') THEN
        EXECUTE 'ALTER TABLE admin_operators OWNER TO helix';
        EXECUTE 'ALTER TABLE app_config_entries OWNER TO helix';
        EXECUTE 'ALTER TABLE app_config_revisions OWNER TO helix';
        EXECUTE 'ALTER SEQUENCE IF EXISTS app_config_revisions_id_seq OWNER TO helix';
    END IF;
END $$;
