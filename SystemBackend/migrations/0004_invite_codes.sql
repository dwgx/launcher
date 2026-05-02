-- 邀请制注册：register 必须带一个有效 invite_code。
--
-- 字段含义：
--   code         8 位 base32 (避免相似字符)，全程大写
--   created_by   admin 标识；自动生成的写 'admin' 即可
--   max_uses     一码可用次数（默认 1，可生成多次用的码）
--   use_count    已用次数；满了就拒
--   expires_at   可选，到期自动失效
--   used_by      最近一次使用者（仅记录最后一个，多用码用 invite_code_users 表展开）
--
-- 用过的码不删除，只标 use_count 满；保留审计。

CREATE TABLE IF NOT EXISTS invite_codes (
    code         TEXT PRIMARY KEY,
    note         TEXT,
    max_uses     INTEGER NOT NULL DEFAULT 1,
    use_count    INTEGER NOT NULL DEFAULT 0,
    created_by   TEXT,
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at   TIMESTAMPTZ,
    used_at      TIMESTAMPTZ,
    used_by      UUID REFERENCES users(id) ON DELETE SET NULL,
    revoked_at   TIMESTAMPTZ,
    revoked_by   TEXT
);
CREATE INDEX IF NOT EXISTS idx_invite_codes_created ON invite_codes(created_at DESC);

-- users 加一个字段记录注册时用的邀请码（即使邀请码后来被删，用户档案里仍知道来源）
ALTER TABLE users
    ADD COLUMN IF NOT EXISTS invite_code_used TEXT;

-- 多用码扩展表：N 用码每次成功消费 +1 行
CREATE TABLE IF NOT EXISTS invite_code_uses (
    id          BIGSERIAL PRIMARY KEY,
    code        TEXT NOT NULL REFERENCES invite_codes(code) ON DELETE CASCADE,
    user_id     UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    used_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    remote_ip   INET
);
CREATE INDEX IF NOT EXISTS idx_invite_code_uses_code ON invite_code_uses(code);

-- 默认插一个超级邀请码方便 dwgx 第一个用户注册（用过即失效）
-- 8 位 base32: 1ST-USER 风格，全大写
INSERT INTO invite_codes (code, note, max_uses, created_by)
    VALUES ('LAUNCHER1', 'bootstrap admin / first user', 1, 'system')
ON CONFLICT DO NOTHING;
