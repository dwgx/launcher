-- HWID 重绑定审批工作流。
-- 用户硬件改动（换主板/CPU/重装系统）→ 客户端发起 rebind request →
-- admin 在面板审批 → approved 后下次登录绑定新 HWID。

CREATE TABLE IF NOT EXISTS hwid_rebind_requests (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id         UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    old_fingerprint TEXT,                          -- 原绑定的 sha256 hex
    new_fingerprint TEXT NOT NULL,
    parts_diff      JSONB NOT NULL,                -- {part_name: {old, new}}
    parts_full      JSONB NOT NULL,                -- 客户端上报的完整 parts，便于 admin 看
    user_reason     TEXT,                          -- 用户填的理由
    status          TEXT NOT NULL DEFAULT 'pending',  -- pending / approved / denied / cancelled
    submitted_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    reviewed_at     TIMESTAMPTZ,
    reviewer        TEXT,                          -- admin 用户名
    review_note     TEXT
);
CREATE INDEX IF NOT EXISTS idx_hwid_rebind_user_id ON hwid_rebind_requests(user_id);
CREATE INDEX IF NOT EXISTS idx_hwid_rebind_status ON hwid_rebind_requests(status, submitted_at DESC);

-- 把 users.hwid_bound 重命名概念：
-- "current" hwid，可由审批后的 rebind 替换。原数据兼容，无需迁移。
ALTER TABLE users
    ADD COLUMN IF NOT EXISTS hwid_strict_required BOOLEAN NOT NULL DEFAULT TRUE,
    ADD COLUMN IF NOT EXISTS hwid_last_changed_at TIMESTAMPTZ;
