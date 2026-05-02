-- 官方频道：固定写死，admin SSR 可重命名 / 清空消息但不能删除。
-- 每个频道有 slug（id 之外的稳定 key），客户端按 slug 查。

ALTER TABLE chats
    ADD COLUMN IF NOT EXISTS slug TEXT,
    ADD COLUMN IF NOT EXISTS is_official BOOLEAN NOT NULL DEFAULT FALSE,
    ADD COLUMN IF NOT EXISTS group_label TEXT;     -- IMPORTANT/GENERAL/GAMES/SHOP

CREATE UNIQUE INDEX IF NOT EXISTS idx_chats_slug_unique ON chats(slug) WHERE slug IS NOT NULL;

-- 写死的官方频道（与 Preview chat_view.inl kChannels 一一对应，剔除 touhou/vrchat）
INSERT INTO chats (kind, title, slug, is_official, group_label, is_public, last_message_at)
SELECT v.kind, v.title, v.slug, TRUE, v.group_label, TRUE, now()
FROM (VALUES
    ('channel', 'announcements', 'announcements', 'IMPORTANT'),
    ('channel', 'rules',         'rules',         'IMPORTANT'),
    ('channel', 'general',       'general',       'GENERAL'),
    ('channel', 'random',        'random',        'GENERAL'),
    ('channel', 'helpdesk',      'helpdesk',      'GENERAL'),
    ('channel', 'cs2',           'cs2',           'GAMES'),
    ('channel', 'market',        'market',        'SHOP'),
    ('channel', 'trades',        'trades',        'SHOP')
) AS v(kind, title, slug, group_label)
WHERE NOT EXISTS (SELECT 1 FROM chats c WHERE c.slug = v.slug);
