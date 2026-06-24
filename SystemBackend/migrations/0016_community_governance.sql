-- Community governance follow-up: per-user announcement state and category admin metadata.

ALTER TABLE ticket_categories
    ADD COLUMN IF NOT EXISTS updated_at TIMESTAMPTZ NOT NULL DEFAULT now();

CREATE TABLE IF NOT EXISTS announcement_reads (
    announcement_id UUID NOT NULL REFERENCES announcements(id) ON DELETE CASCADE,
    user_id         UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    read_at         TIMESTAMPTZ NOT NULL DEFAULT now(),
    acknowledged_at TIMESTAMPTZ,
    PRIMARY KEY (announcement_id, user_id)
);

CREATE INDEX IF NOT EXISTS idx_announcement_reads_user
    ON announcement_reads(user_id, read_at DESC);

CREATE INDEX IF NOT EXISTS idx_message_admin_edits_editor
    ON message_admin_edits(editor_id, created_at DESC);

DO $$ BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'helix') THEN
        EXECUTE 'ALTER TABLE announcement_reads OWNER TO helix';
    END IF;
END $$;
