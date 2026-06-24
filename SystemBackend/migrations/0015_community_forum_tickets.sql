-- Community/forum/ticket foundation. This migration is additive and keeps the
-- existing chat APIs compatible.

CREATE TABLE IF NOT EXISTS community_areas (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    slug        TEXT NOT NULL UNIQUE,
    name        TEXT NOT NULL,
    description TEXT NOT NULL DEFAULT '',
    sort_order  INTEGER NOT NULL DEFAULT 100,
    is_enabled  BOOLEAN NOT NULL DEFAULT TRUE,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS channel_settings (
    chat_id               UUID PRIMARY KEY REFERENCES chats(id) ON DELETE CASCADE,
    area_id               UUID REFERENCES community_areas(id) ON DELETE SET NULL,
    display_name          TEXT,
    write_policy          TEXT NOT NULL DEFAULT 'everyone'
        CHECK (write_policy IN ('everyone','admin_only','role_only','min_level','readonly','locked','subscriber_only')),
    allowed_role          TEXT,
    min_level             INTEGER NOT NULL DEFAULT 1 CHECK (min_level BETWEEN 1 AND 100),
    slowmode_seconds      INTEGER NOT NULL DEFAULT 0 CHECK (slowmode_seconds BETWEEN 0 AND 86400),
    requires_subscription BOOLEAN NOT NULL DEFAULT FALSE,
    is_readonly           BOOLEAN NOT NULL DEFAULT FALSE,
    is_locked             BOOLEAN NOT NULL DEFAULT FALSE,
    is_enabled            BOOLEAN NOT NULL DEFAULT TRUE,
    sort_order            INTEGER NOT NULL DEFAULT 100,
    created_at            TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at            TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS ticket_categories (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    slug        TEXT NOT NULL UNIQUE,
    name        TEXT NOT NULL,
    description TEXT NOT NULL DEFAULT '',
    is_builtin  BOOLEAN NOT NULL DEFAULT FALSE,
    is_enabled  BOOLEAN NOT NULL DEFAULT TRUE,
    sort_order  INTEGER NOT NULL DEFAULT 100,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS tickets (
    id                        UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    chat_id                   UUID NOT NULL UNIQUE REFERENCES chats(id) ON DELETE CASCADE,
    number                    BIGSERIAL UNIQUE,
    creator_id                UUID REFERENCES users(id) ON DELETE SET NULL,
    category_id               UUID REFERENCES ticket_categories(id) ON DELETE SET NULL,
    title                     TEXT NOT NULL,
    status                    TEXT NOT NULL DEFAULT 'open'
        CHECK (status IN ('open','pending','resolved','closed')),
    visibility                TEXT NOT NULL DEFAULT 'private'
        CHECK (visibility IN ('private','public')),
    protected_by_superadmin   BOOLEAN NOT NULL DEFAULT FALSE,
    protected_reason          TEXT,
    protected_at              TIMESTAMPTZ,
    protected_by              UUID REFERENCES users(id) ON DELETE SET NULL,
    assigned_admin_id         UUID REFERENCES users(id) ON DELETE SET NULL,
    resolved_by               UUID REFERENCES users(id) ON DELETE SET NULL,
    resolved_at               TIMESTAMPTZ,
    created_at                TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at                TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_tickets_creator ON tickets(creator_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_tickets_visibility ON tickets(visibility, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_tickets_status ON tickets(status, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_tickets_protected ON tickets(protected_by_superadmin, created_at DESC);

CREATE TABLE IF NOT EXISTS forum_topics (
    id            UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    chat_id       UUID NOT NULL UNIQUE REFERENCES chats(id) ON DELETE CASCADE,
    area_id       UUID REFERENCES community_areas(id) ON DELETE SET NULL,
    author_id     UUID REFERENCES users(id) ON DELETE SET NULL,
    title         TEXT NOT NULL,
    status        TEXT NOT NULL DEFAULT 'open'
        CHECK (status IN ('open','locked','hidden','archived')),
    pinned        BOOLEAN NOT NULL DEFAULT FALSE,
    locked        BOOLEAN NOT NULL DEFAULT FALSE,
    view_count    BIGINT NOT NULL DEFAULT 0,
    last_reply_at TIMESTAMPTZ,
    created_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at    TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_forum_topics_area ON forum_topics(area_id, pinned DESC, updated_at DESC);
CREATE INDEX IF NOT EXISTS idx_forum_topics_chat ON forum_topics(chat_id);

CREATE TABLE IF NOT EXISTS announcements (
    id            UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    title         TEXT NOT NULL,
    body          TEXT NOT NULL,
    severity      TEXT NOT NULL DEFAULT 'normal'
        CHECK (severity IN ('normal','important','critical')),
    audience_role TEXT,
    min_level     INTEGER NOT NULL DEFAULT 1 CHECK (min_level BETWEEN 1 AND 100),
    force_popup   BOOLEAN NOT NULL DEFAULT FALSE,
    red_dot       BOOLEAN NOT NULL DEFAULT TRUE,
    starts_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at    TIMESTAMPTZ,
    created_by    UUID REFERENCES users(id) ON DELETE SET NULL,
    created_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at    TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_announcements_active ON announcements(starts_at, expires_at, severity);

CREATE TABLE IF NOT EXISTS user_progress (
    user_id          UUID PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
    level            INTEGER NOT NULL DEFAULT 1 CHECK (level BETWEEN 1 AND 100),
    xp               BIGINT NOT NULL DEFAULT 0 CHECK (xp >= 0),
    last_checkin_at  TIMESTAMPTZ,
    updated_at       TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS user_xp_events (
    id          BIGSERIAL PRIMARY KEY,
    user_id     UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    event_type  TEXT NOT NULL,
    amount      INTEGER NOT NULL,
    source_type TEXT,
    source_id   TEXT,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_user_xp_events_user ON user_xp_events(user_id, created_at DESC);

CREATE TABLE IF NOT EXISTS message_admin_edits (
    id          BIGSERIAL PRIMARY KEY,
    message_id  BIGINT NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
    editor_id   UUID REFERENCES users(id) ON DELETE SET NULL,
    old_payload JSONB NOT NULL,
    new_payload JSONB NOT NULL,
    reason      TEXT NOT NULL DEFAULT '',
    edit_kind   TEXT NOT NULL DEFAULT 'edit' CHECK (edit_kind IN ('edit','redact')),
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_message_admin_edits_message ON message_admin_edits(message_id, created_at DESC);

CREATE TABLE IF NOT EXISTS chat_mutes (
    id             BIGSERIAL PRIMARY KEY,
    chat_id        UUID NOT NULL REFERENCES chats(id) ON DELETE CASCADE,
    target_user_id UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    muted_by       UUID REFERENCES users(id) ON DELETE SET NULL,
    reason         TEXT NOT NULL DEFAULT '',
    muted_until    TIMESTAMPTZ NOT NULL,
    revoked_at     TIMESTAMPTZ,
    revoked_by     UUID REFERENCES users(id) ON DELETE SET NULL,
    revoke_reason  TEXT,
    created_at     TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_chat_mutes_active
    ON chat_mutes(chat_id, target_user_id, muted_until DESC)
    WHERE revoked_at IS NULL;

INSERT INTO community_areas (slug, name, description, sort_order) VALUES
    ('important', 'IMPORTANT', 'Official announcements and rules', 10),
    ('general', 'GENERAL', 'General community discussion', 20),
    ('games', 'GAMES', 'Game discussion boards', 30),
    ('shop', 'SHOP', 'Market and trade boards', 40),
    ('tickets', 'Tickets', 'Support tickets and public issue discussions', 50),
    ('forum', 'Forum', 'Long-form community posts', 60)
ON CONFLICT (slug) DO NOTHING;

INSERT INTO ticket_categories (slug, name, description, is_builtin, sort_order) VALUES
    ('hwid_binding', 'HWID 绑定', 'HWID binding and rebind issues', TRUE, 10),
    ('purchase_subscription', '购买/订阅', 'Purchase and subscription issues', TRUE, 20),
    ('feedback', '问题反馈', 'Bug reports and product feedback', TRUE, 30)
ON CONFLICT (slug) DO NOTHING;

INSERT INTO channel_settings (
    chat_id, area_id, display_name, write_policy, min_level, slowmode_seconds,
    requires_subscription, is_readonly, is_locked, sort_order
)
SELECT
    c.id,
    a.id,
    c.title,
    CASE WHEN c.write_role = 'admin_only' THEN 'admin_only' ELSE 'everyone' END,
    1,
    0,
    FALSE,
    FALSE,
    FALSE,
    CASE c.slug
        WHEN 'announcements' THEN 10
        WHEN 'rules' THEN 20
        WHEN 'general' THEN 30
        WHEN 'random' THEN 40
        WHEN 'helpdesk' THEN 50
        WHEN 'cs2' THEN 60
        WHEN 'market' THEN 70
        WHEN 'trades' THEN 80
        ELSE 100
    END
FROM chats c
LEFT JOIN community_areas a ON a.slug = lower(COALESCE(c.group_label, 'forum'))
WHERE c.kind = 'channel'
ON CONFLICT (chat_id) DO NOTHING;

INSERT INTO user_progress (user_id)
SELECT id FROM users
ON CONFLICT (user_id) DO NOTHING;

DO $$ BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'helix') THEN
        EXECUTE 'ALTER TABLE community_areas OWNER TO helix';
        EXECUTE 'ALTER TABLE channel_settings OWNER TO helix';
        EXECUTE 'ALTER TABLE ticket_categories OWNER TO helix';
        EXECUTE 'ALTER TABLE tickets OWNER TO helix';
        EXECUTE 'ALTER SEQUENCE IF EXISTS tickets_number_seq OWNER TO helix';
        EXECUTE 'ALTER TABLE forum_topics OWNER TO helix';
        EXECUTE 'ALTER TABLE announcements OWNER TO helix';
        EXECUTE 'ALTER TABLE user_progress OWNER TO helix';
        EXECUTE 'ALTER TABLE user_xp_events OWNER TO helix';
        EXECUTE 'ALTER SEQUENCE IF EXISTS user_xp_events_id_seq OWNER TO helix';
        EXECUTE 'ALTER TABLE message_admin_edits OWNER TO helix';
        EXECUTE 'ALTER SEQUENCE IF EXISTS message_admin_edits_id_seq OWNER TO helix';
        EXECUTE 'ALTER TABLE chat_mutes OWNER TO helix';
        EXECUTE 'ALTER SEQUENCE IF EXISTS chat_mutes_id_seq OWNER TO helix';
    END IF;
END $$;
