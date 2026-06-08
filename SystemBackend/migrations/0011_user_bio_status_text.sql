-- Profile text fields used by /api/profile/update and /api/profile/peer/:key.
ALTER TABLE users
    ADD COLUMN IF NOT EXISTS status_text TEXT NOT NULL DEFAULT '',
    ADD COLUMN IF NOT EXISTS bio         TEXT NOT NULL DEFAULT '';

DO $$ BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'helix') THEN
        EXECUTE 'ALTER TABLE users OWNER TO helix';
    END IF;
END $$;
