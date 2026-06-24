-- Keep this separate from 0017 because that migration may already be applied
-- on live deployments. 0017's CREATE TABLE has user_id UNIQUE for fresh
-- installs; this backfills the same invariant for older/pre-created tables.

CREATE UNIQUE INDEX IF NOT EXISTS idx_admin_operators_user_id_unique
    ON admin_operators(user_id)
    WHERE user_id IS NOT NULL;
