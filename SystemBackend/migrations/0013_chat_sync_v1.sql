-- Chat sync v1: idempotent sends, replayable events, and user client settings.

ALTER TABLE messages
    ADD COLUMN IF NOT EXISTS client_msg_id UUID,
    ADD COLUMN IF NOT EXISTS sender_device_id TEXT;

CREATE UNIQUE INDEX IF NOT EXISTS ux_messages_client_msg
    ON messages(chat_id, sender_id, client_msg_id)
    WHERE client_msg_id IS NOT NULL AND sender_id IS NOT NULL;

CREATE INDEX IF NOT EXISTS idx_messages_client_msg
    ON messages(client_msg_id)
    WHERE client_msg_id IS NOT NULL;

CREATE TABLE IF NOT EXISTS chat_events (
    id          BIGSERIAL PRIMARY KEY,
    chat_id     UUID REFERENCES chats(id) ON DELETE CASCADE,
    event_type  TEXT NOT NULL,
    message_id  BIGINT REFERENCES messages(id) ON DELETE SET NULL,
    actor_id    UUID REFERENCES users(id) ON DELETE SET NULL,
    payload     JSONB NOT NULL DEFAULT '{}'::jsonb,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS idx_chat_events_chat_id
    ON chat_events(chat_id, id);

CREATE INDEX IF NOT EXISTS idx_chat_events_id
    ON chat_events(id);

CREATE TABLE IF NOT EXISTS user_client_settings (
    user_id     UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    scope       TEXT NOT NULL DEFAULT 'desktop',
    key         TEXT NOT NULL,
    value       JSONB NOT NULL,
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (user_id, scope, key)
);

CREATE INDEX IF NOT EXISTS idx_user_client_settings_user_scope
    ON user_client_settings(user_id, scope);
