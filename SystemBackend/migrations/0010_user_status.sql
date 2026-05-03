-- 用户状态 (online / busy / away / sleep / offline)
-- 用于在客户端互显头像底角的小绿/红/橙/紫圆点。
-- last_seen 由 heartbeat 周期写入，离线判断 = now() - last_seen > 30s。

ALTER TABLE users
    ADD COLUMN IF NOT EXISTS status     TEXT NOT NULL DEFAULT 'online',
    ADD COLUMN IF NOT EXISTS last_seen  TIMESTAMPTZ NOT NULL DEFAULT now();

CREATE INDEX IF NOT EXISTS idx_users_last_seen ON users(last_seen);

DO $$ BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'helix') THEN
        EXECUTE 'ALTER TABLE users OWNER TO helix';
    END IF;
END $$;
