-- 用户头衔（role）— 用户原话 "admin / newhand / oldhand / user 这种可以预设也可以自定义，支持中文显示"
--
-- users.role         — 角色 id（系统识别用）
-- users.role_label   — 中文显示头衔（admin SSR 可改）
-- users.is_admin 已存在（0008），保留兼容；admin role 隐含 is_admin = TRUE
-- 默认 4 个预设：admin / oldhand / newhand / user
-- admin SSR 可加自定义角色（写到 chats.write_role 等 enum 还是 admin_only/user，
-- 但 users.role 任意 string）

ALTER TABLE users
    ADD COLUMN IF NOT EXISTS role TEXT NOT NULL DEFAULT 'user',
    ADD COLUMN IF NOT EXISTS role_label TEXT;

-- 已经是 admin 的用户保持 admin role
UPDATE users SET role = 'admin', role_label = '管理员'
    WHERE is_admin = TRUE AND role = 'user';

-- 给一个角色目录表（可加自定义角色，admin SSR 用）
CREATE TABLE IF NOT EXISTS user_roles_catalog (
    role        TEXT PRIMARY KEY,
    label_zh    TEXT NOT NULL,
    sort_order  INTEGER NOT NULL DEFAULT 100,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

INSERT INTO user_roles_catalog (role, label_zh, sort_order) VALUES
    ('admin',   '管理员',  1),
    ('oldhand', '老司机',  10),
    ('newhand', '萌新',    20),
    ('user',    '用户',    30)
ON CONFLICT (role) DO NOTHING;

-- 个人标签 (chips)
CREATE TABLE IF NOT EXISTS user_tags (
    user_id   UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    tag       TEXT NOT NULL,
    sort_order INTEGER NOT NULL DEFAULT 100,
    PRIMARY KEY (user_id, tag)
);

-- 兜底：把表 owner 改成 helix（migration 由 postgres 角色应用时
-- 创建出的表默认 owner 是 postgres，systembackend 服务用 helix 跑会 permission denied）
DO $$ BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'helix') THEN
        EXECUTE 'ALTER TABLE user_roles_catalog OWNER TO helix';
        EXECUTE 'ALTER TABLE user_tags OWNER TO helix';
    END IF;
END $$;
