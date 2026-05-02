-- 频道写权限 + 用户角色
--   chats.write_role:
--     'admin_only' → 只 admin 能发 (announcements / rules / helpdesk / cs2 / market / trades)
--     'user'       → 任何登录用户可发 (general / random — 用户聊天的 2 个地方)
--   users.is_admin: bool，admin SSR 可勾选

ALTER TABLE chats
    ADD COLUMN IF NOT EXISTS write_role TEXT NOT NULL DEFAULT 'admin_only'
    CHECK (write_role IN ('admin_only', 'user'));

ALTER TABLE users
    ADD COLUMN IF NOT EXISTS is_admin BOOLEAN NOT NULL DEFAULT FALSE;

-- 默认权限：general / random 是用户聊天频道；其他官方频道只 admin 发
UPDATE chats SET write_role = 'user'
    WHERE slug IN ('general', 'random');
UPDATE chats SET write_role = 'admin_only'
    WHERE slug IN ('announcements', 'rules', 'helpdesk', 'cs2', 'market', 'trades');
