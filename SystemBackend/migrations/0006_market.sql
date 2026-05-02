-- Market 地基：用户卖虚拟物品（订阅码、表情包、皮肤、配置文件）。
-- 现金/支付集成留待 Phase 8；现在只支持虚拟币 credit 字段。

-- 用户 credit 余额
ALTER TABLE users
    ADD COLUMN IF NOT EXISTS credit_balance BIGINT NOT NULL DEFAULT 0;  -- 单位：分 (1.00 元 = 100)

-- 类目
CREATE TABLE IF NOT EXISTS market_categories (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    slug        TEXT NOT NULL UNIQUE,             -- "stickers" / "configs" / "saves" / "themes"
    name        TEXT NOT NULL,
    icon        TEXT,                              -- emoji 或 svg 引用
    sort_order  INTEGER NOT NULL DEFAULT 0
);

-- 商品
CREATE TABLE IF NOT EXISTS market_listings (
    id            UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    category_id   UUID NOT NULL REFERENCES market_categories(id) ON DELETE RESTRICT,
    seller_id     UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    title         TEXT NOT NULL,
    description   TEXT,
    price_cents   BIGINT NOT NULL DEFAULT 0,        -- 0 = 免费
    cover_media_id BIGINT REFERENCES media_files(id) ON DELETE SET NULL,

    -- 商品类型 + 引用：内容引用具体表
    item_type     TEXT NOT NULL CHECK (item_type IN ('sticker_pack','config','save','theme','other')),
    item_ref_id   UUID,                              -- 指向 sticker_packs.id 等

    status        TEXT NOT NULL DEFAULT 'active' CHECK (status IN ('draft','active','paused','removed')),
    purchase_count INTEGER NOT NULL DEFAULT 0,
    rating_avg    REAL NOT NULL DEFAULT 0,
    rating_count  INTEGER NOT NULL DEFAULT 0,
    created_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at    TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_listings_category ON market_listings(category_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_listings_seller   ON market_listings(seller_id);
CREATE INDEX IF NOT EXISTS idx_listings_status   ON market_listings(status, purchase_count DESC);

-- 订单
CREATE TABLE IF NOT EXISTS market_orders (
    id            UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    listing_id    UUID NOT NULL REFERENCES market_listings(id) ON DELETE RESTRICT,
    buyer_id      UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    seller_id     UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    price_cents   BIGINT NOT NULL,
    status        TEXT NOT NULL DEFAULT 'pending' CHECK (status IN
        ('pending','paid','delivered','refunded','cancelled')),
    created_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    paid_at       TIMESTAMPTZ,
    delivered_at  TIMESTAMPTZ
);
CREATE INDEX IF NOT EXISTS idx_orders_buyer  ON market_orders(buyer_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_orders_seller ON market_orders(seller_id, created_at DESC);

-- 评价
CREATE TABLE IF NOT EXISTS market_reviews (
    id            UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    listing_id    UUID NOT NULL REFERENCES market_listings(id) ON DELETE CASCADE,
    order_id      UUID REFERENCES market_orders(id) ON DELETE SET NULL,
    reviewer_id   UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    rating        SMALLINT NOT NULL CHECK (rating BETWEEN 1 AND 5),
    body          TEXT,
    created_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (listing_id, reviewer_id)
);
CREATE INDEX IF NOT EXISTS idx_reviews_listing ON market_reviews(listing_id, created_at DESC);

-- credit 流水（充值/消费/退款）
CREATE TABLE IF NOT EXISTS credit_ledger (
    id          BIGSERIAL PRIMARY KEY,
    user_id     UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    delta_cents BIGINT NOT NULL,
    reason      TEXT NOT NULL,                       -- 'recharge' / 'purchase' / 'refund' / 'admin_grant'
    related_order_id UUID REFERENCES market_orders(id) ON DELETE SET NULL,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_ledger_user ON credit_ledger(user_id, created_at DESC);

-- 默认类目
INSERT INTO market_categories (slug, name, icon, sort_order) VALUES
    ('stickers', '表情包', '✨', 10),
    ('configs',  '配置',   '⚙', 20),
    ('saves',    '存档',   '💾', 30),
    ('themes',   '主题',   '🎨', 40),
    ('other',    '其他',   '⋯', 99)
ON CONFLICT DO NOTHING;
