-- Global chat moderation and message mention metadata.

CREATE TABLE IF NOT EXISTS user_mutes (
    id             BIGSERIAL PRIMARY KEY,
    target_user_id UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    muted_by       UUID REFERENCES users(id) ON DELETE SET NULL,
    reason         TEXT NOT NULL DEFAULT '',
    muted_until    TIMESTAMPTZ NOT NULL,
    revoked_at     TIMESTAMPTZ,
    revoked_by     UUID REFERENCES users(id) ON DELETE SET NULL,
    revoke_reason  TEXT,
    created_at     TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS idx_user_mutes_target_active
    ON user_mutes(target_user_id, muted_until DESC)
    WHERE revoked_at IS NULL;

CREATE INDEX IF NOT EXISTS idx_user_mutes_moderator
    ON user_mutes(muted_by, created_at DESC);

CREATE TABLE IF NOT EXISTS message_mentions (
    message_id BIGINT NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
    user_id    UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (message_id, user_id)
);

CREATE INDEX IF NOT EXISTS idx_message_mentions_user
    ON message_mentions(user_id, message_id DESC);

INSERT INTO user_roles_catalog (role, label_zh, sort_order) VALUES
    ('owner', '超级管理员', 0),
    ('super_admin', '超级管理员', 1)
ON CONFLICT (role) DO NOTHING;

DO $$ BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'helix') THEN
        EXECUTE 'ALTER TABLE user_mutes OWNER TO helix';
        EXECUTE 'ALTER TABLE message_mentions OWNER TO helix';
    END IF;
END $$;
