-- Keep table ownership portable across production and audit databases.
-- Earlier migrations are already checksum-tracked, so owner fixes must live here.
DO $$
DECLARE
    db_owner TEXT;
BEGIN
    SELECT pg_catalog.pg_get_userbyid(datdba)
      INTO db_owner
      FROM pg_catalog.pg_database
      WHERE datname = current_database();

    IF db_owner IS NOT NULL THEN
        EXECUTE format('ALTER TABLE users OWNER TO %I', db_owner);
        EXECUTE format('ALTER TABLE user_roles_catalog OWNER TO %I', db_owner);
        EXECUTE format('ALTER TABLE user_tags OWNER TO %I', db_owner);
    END IF;
END $$;
