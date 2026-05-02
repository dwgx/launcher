-- Telegram 风格通讯地基。三种会话：dm (1v1) / group / channel (广播)。
--
-- 媒体策略：原始文件存 /opt/systembackend/media/<sha256[:2]>/<sha256>.<ext>，
-- 数据库只存 sha256 + 元数据；同样内容自动去重 (sha256 unique)。
-- 大视频走 chunked upload + finalize 后入 attachments；本地 5GB 上限。

-- ---------- 会话 ----------
CREATE TABLE IF NOT EXISTS chats (
    id           UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    kind         TEXT NOT NULL CHECK (kind IN ('dm','group','channel')),
    title        TEXT,                          -- group/channel 必填，dm 自动从对端 nickname
    avatar_path  TEXT,                          -- /opt/systembackend/media/...
    created_by   UUID REFERENCES users(id) ON DELETE SET NULL,
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_message_at TIMESTAMPTZ,                -- 用于排序，降序就是 chat 列表
    is_public    BOOLEAN NOT NULL DEFAULT FALSE -- channel 用，公开可被搜索
);
CREATE INDEX IF NOT EXISTS idx_chats_last_message ON chats(last_message_at DESC NULLS LAST);

-- DM 会话两个 user_id 排序后做 unique key 防重复
CREATE TABLE IF NOT EXISTS chat_dm_index (
    chat_id   UUID PRIMARY KEY REFERENCES chats(id) ON DELETE CASCADE,
    user_lo   UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    user_hi   UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    UNIQUE (user_lo, user_hi),
    CHECK (user_lo < user_hi)
);

-- ---------- 成员 ----------
CREATE TABLE IF NOT EXISTS chat_members (
    chat_id     UUID NOT NULL REFERENCES chats(id) ON DELETE CASCADE,
    user_id     UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    role        TEXT NOT NULL DEFAULT 'member' CHECK (role IN ('member','admin','owner')),
    joined_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_read_message_id BIGINT,
    muted_until TIMESTAMPTZ,
    PRIMARY KEY (chat_id, user_id)
);
CREATE INDEX IF NOT EXISTS idx_chat_members_user ON chat_members(user_id);

-- ---------- 消息 ----------
-- type:
--   text       - payload.text
--   image      - payload.attachment_id
--   video      - payload.attachment_id (+ thumb_attachment_id 可选)
--   gif        - payload.attachment_id
--   sticker    - payload.sticker_id
--   pack_share - payload.sticker_pack_id (分享一个表情包)
--   reply      - reply_to_id 指向被回复的消息（其他字段同正常消息）
--   system     - 系统消息（加群/退群/改群名）
CREATE TABLE IF NOT EXISTS messages (
    id           BIGSERIAL PRIMARY KEY,
    chat_id      UUID NOT NULL REFERENCES chats(id) ON DELETE CASCADE,
    sender_id    UUID REFERENCES users(id) ON DELETE SET NULL,
    msg_type     TEXT NOT NULL CHECK (msg_type IN
        ('text','image','video','gif','sticker','pack_share','system')),
    payload      JSONB NOT NULL DEFAULT '{}'::jsonb,
    reply_to_id  BIGINT REFERENCES messages(id) ON DELETE SET NULL,
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    edited_at    TIMESTAMPTZ,
    deleted_at   TIMESTAMPTZ                       -- 软删除
);
CREATE INDEX IF NOT EXISTS idx_messages_chat_time ON messages(chat_id, id DESC);
CREATE INDEX IF NOT EXISTS idx_messages_sender    ON messages(sender_id);

-- ---------- 反应 (Telegram 风格 emoji react) ----------
CREATE TABLE IF NOT EXISTS message_reactions (
    message_id  BIGINT NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
    user_id     UUID   NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    emoji       TEXT   NOT NULL,
    reacted_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (message_id, user_id, emoji)
);

-- ---------- 媒体（image/video/gif 文件） ----------
CREATE TABLE IF NOT EXISTS media_files (
    id           BIGSERIAL PRIMARY KEY,
    sha256       TEXT NOT NULL UNIQUE,             -- 内容寻址，去重
    mime         TEXT NOT NULL,
    size_bytes   BIGINT NOT NULL,
    width        INTEGER,                          -- 图/视频
    height       INTEGER,
    duration_ms  INTEGER,                          -- 视频/gif
    relative_path TEXT NOT NULL,                   -- 相对 media_root，例如 'a3/a3f1...d2.png'
    uploader_id  UUID REFERENCES users(id) ON DELETE SET NULL,
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_media_uploader ON media_files(uploader_id);

-- ---------- 表情包 ----------
-- sticker = 一个静态/动态图（image/gif/webp/lottie）
CREATE TABLE IF NOT EXISTS stickers (
    id            UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    media_id      BIGINT NOT NULL REFERENCES media_files(id) ON DELETE CASCADE,
    emoji_alias   TEXT,                              -- 类似 :smile: 或 emoji 字符
    label         TEXT,                              -- 用户给的名字
    creator_id    UUID REFERENCES users(id) ON DELETE SET NULL,
    is_animated   BOOLEAN NOT NULL DEFAULT FALSE,
    created_at    TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_stickers_creator ON stickers(creator_id);

-- 表情包（用户自己组合 sticker 成一个 pack）
CREATE TABLE IF NOT EXISTS sticker_packs (
    id           UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name         TEXT NOT NULL,
    short_name   TEXT UNIQUE,                        -- 类似 telegram 的 t.me/addstickers/<short_name>
    description  TEXT,
    cover_media_id BIGINT REFERENCES media_files(id) ON DELETE SET NULL,
    creator_id   UUID REFERENCES users(id) ON DELETE SET NULL,
    is_public    BOOLEAN NOT NULL DEFAULT TRUE,
    install_count INTEGER NOT NULL DEFAULT 0,
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at   TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_packs_creator ON sticker_packs(creator_id);
CREATE INDEX IF NOT EXISTS idx_packs_public ON sticker_packs(is_public, install_count DESC);

-- pack <-> sticker 多对多 + 排序
CREATE TABLE IF NOT EXISTS sticker_pack_items (
    pack_id      UUID NOT NULL REFERENCES sticker_packs(id) ON DELETE CASCADE,
    sticker_id   UUID NOT NULL REFERENCES stickers(id) ON DELETE CASCADE,
    sort_order   INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (pack_id, sticker_id)
);
CREATE INDEX IF NOT EXISTS idx_pack_items_order ON sticker_pack_items(pack_id, sort_order);

-- 用户安装的表情包
CREATE TABLE IF NOT EXISTS user_sticker_packs (
    user_id      UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    pack_id      UUID NOT NULL REFERENCES sticker_packs(id) ON DELETE CASCADE,
    sort_order   INTEGER NOT NULL DEFAULT 0,
    installed_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (user_id, pack_id)
);

-- 触发器：messages 插入时更新 chats.last_message_at（手写 update 也行；这里加触发器更稳）
CREATE OR REPLACE FUNCTION bump_chat_last_message() RETURNS TRIGGER AS $$
BEGIN
    UPDATE chats SET last_message_at = NEW.created_at WHERE id = NEW.chat_id;
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;
DROP TRIGGER IF EXISTS trg_bump_chat ON messages;
CREATE TRIGGER trg_bump_chat AFTER INSERT ON messages
    FOR EACH ROW EXECUTE FUNCTION bump_chat_last_message();
