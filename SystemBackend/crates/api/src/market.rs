// Market 地基：分类 / 商品列表+详情+创建 / 订单（虚拟币 credit_balance）

use crate::media::auth_user;
use crate::state::AppState;
use axum::{
    extract::{Json, Path, Query, State},
    http::{HeaderMap, StatusCode},
};
use serde::{Deserialize, Serialize};
use std::sync::Arc;
use uuid::Uuid;

use crate::error::internal;

// ---------- 分类 ----------
#[derive(Serialize)]
pub struct Category {
    pub id: String,
    pub slug: String,
    pub name: String,
    pub icon: Option<String>,
    pub sort_order: i32,
}

pub async fn list_categories(
    State(s): State<Arc<AppState>>,
) -> Result<Json<Vec<Category>>, (StatusCode, String)> {
    let rows = sqlx::query!(
        "SELECT id, slug, name, icon, sort_order FROM market_categories ORDER BY sort_order"
    )
    .fetch_all(&s.db)
    .await
    .map_err(internal)?;
    Ok(Json(
        rows.into_iter()
            .map(|r| Category {
                id: r.id.to_string(),
                slug: r.slug,
                name: r.name,
                icon: r.icon,
                sort_order: r.sort_order,
            })
            .collect(),
    ))
}

// ---------- 列表 / 搜索 ----------
#[derive(Deserialize)]
pub struct ListQ {
    pub category: Option<String>,
    pub q: Option<String>,
    pub limit: Option<i64>,
    pub seller: Option<Uuid>,
}

#[derive(Serialize)]
pub struct ListingBrief {
    pub id: String,
    pub title: String,
    pub price_cents: i64,
    pub category: String,
    pub item_type: String,
    pub cover_url: Option<String>,
    pub purchase_count: i32,
    pub rating_avg: f32,
    pub rating_count: i32,
    pub seller_id: String,
    pub created_at: i64,
}

pub async fn list_listings(
    State(s): State<Arc<AppState>>,
    Query(q): Query<ListQ>,
) -> Result<Json<Vec<ListingBrief>>, (StatusCode, String)> {
    let limit = q.limit.unwrap_or(50).clamp(1, 200);
    let rows = sqlx::query!(
        r#"SELECT l.id, l.title, l.price_cents, l.item_type, l.purchase_count,
                  l.rating_avg, l.rating_count, l.seller_id, l.created_at,
                  c.slug as category_slug,
                  m.sha256 as "cover_sha?", m.mime as "cover_mime?"
           FROM market_listings l
             JOIN market_categories c ON c.id = l.category_id
             LEFT JOIN media_files m  ON m.id = l.cover_media_id
           WHERE l.status = 'active'
             AND ($1::TEXT IS NULL OR c.slug = $1)
             AND ($2::TEXT IS NULL OR l.title ILIKE '%' || $2 || '%')
             AND ($3::UUID IS NULL OR l.seller_id = $3)
           ORDER BY l.purchase_count DESC, l.created_at DESC LIMIT $4"#,
        q.category,
        q.q,
        q.seller,
        limit
    )
    .fetch_all(&s.db)
    .await
    .map_err(internal)?;

    Ok(Json(
        rows.into_iter()
            .map(|r| ListingBrief {
                id: r.id.to_string(),
                title: r.title,
                price_cents: r.price_cents,
                category: r.category_slug,
                item_type: r.item_type,
                purchase_count: r.purchase_count,
                rating_avg: r.rating_avg,
                rating_count: r.rating_count,
                seller_id: r.seller_id.to_string(),
                created_at: r.created_at.timestamp(),
                cover_url: r.cover_sha.zip(r.cover_mime).map(|(sha, mime)| {
                    let ext = match mime.as_str() {
                        "image/png" => "png",
                        "image/jpeg" => "jpg",
                        "image/gif" => "gif",
                        "image/webp" => "webp",
                        _ => "bin",
                    };
                    format!("/api/media/{}/file.{}", sha, ext)
                }),
            })
            .collect(),
    ))
}

// ---------- 详情 ----------
#[derive(Serialize)]
pub struct ListingDetail {
    pub id: String,
    pub title: String,
    pub description: Option<String>,
    pub price_cents: i64,
    pub category: String,
    pub item_type: String,
    pub item_ref_id: Option<String>,
    pub cover_url: Option<String>,
    pub seller_id: String,
    pub purchase_count: i32,
    pub rating_avg: f32,
    pub rating_count: i32,
    pub status: String,
    pub created_at: i64,
}

pub async fn get_listing(
    State(s): State<Arc<AppState>>,
    Path(id): Path<Uuid>,
) -> Result<Json<ListingDetail>, (StatusCode, String)> {
    let r = sqlx::query!(
        r#"SELECT l.id, l.title, l.description, l.price_cents, l.item_type, l.item_ref_id,
                  l.purchase_count, l.rating_avg, l.rating_count, l.seller_id, l.status, l.created_at,
                  c.slug as category_slug,
                  m.sha256 as "cover_sha?", m.mime as "cover_mime?"
           FROM market_listings l
             JOIN market_categories c ON c.id = l.category_id
             LEFT JOIN media_files m  ON m.id = l.cover_media_id
           WHERE l.id = $1 AND l.status = 'active'"#, id)
        .fetch_optional(&s.db).await.map_err(internal)?
        .ok_or((StatusCode::NOT_FOUND, "not found".into()))?;

    Ok(Json(ListingDetail {
        id: r.id.to_string(),
        title: r.title,
        description: r.description,
        price_cents: r.price_cents,
        category: r.category_slug,
        item_type: r.item_type,
        item_ref_id: r.item_ref_id.map(|u| u.to_string()),
        cover_url: r.cover_sha.zip(r.cover_mime).map(|(sha, mime)| {
            let ext = match mime.as_str() {
                "image/png" => "png",
                "image/jpeg" => "jpg",
                "image/gif" => "gif",
                "image/webp" => "webp",
                _ => "bin",
            };
            format!("/api/media/{}/file.{}", sha, ext)
        }),
        seller_id: r.seller_id.to_string(),
        purchase_count: r.purchase_count,
        rating_avg: r.rating_avg,
        rating_count: r.rating_count,
        status: r.status,
        created_at: r.created_at.timestamp(),
    }))
}

// ---------- 创建 ----------
#[derive(Deserialize)]
pub struct CreateReq {
    pub session_token: String,
    pub category_slug: String,
    pub title: String,
    pub description: Option<String>,
    pub price_cents: i64,
    pub item_type: String,
    pub item_ref_id: Option<Uuid>,
    pub cover_media_id: Option<i64>,
}

pub async fn create_listing(
    State(s): State<Arc<AppState>>,
    Json(req): Json<CreateReq>,
) -> Result<Json<serde_json::Value>, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    if req.title.len() > 128 {
        return Err((StatusCode::BAD_REQUEST, "title too long".into()));
    }
    if req.description.as_ref().map_or(false, |d| d.len() > 4000) {
        return Err((StatusCode::BAD_REQUEST, "description too long".into()));
    }
    if req.price_cents < 0 || req.price_cents > 1_000_000_00 {
        return Err((StatusCode::BAD_REQUEST, "price out of range".into()));
    }
    let cat = sqlx::query!(
        "SELECT id FROM market_categories WHERE slug = $1",
        req.category_slug
    )
    .fetch_optional(&s.db)
    .await
    .map_err(internal)?
    .ok_or((StatusCode::BAD_REQUEST, "category not found".into()))?;

    let row = sqlx::query!(
        r#"INSERT INTO market_listings
            (category_id, seller_id, title, description, price_cents, item_type, item_ref_id, cover_media_id)
           VALUES ($1, $2, $3, $4, $5, $6, $7, $8) RETURNING id"#,
        cat.id, me, req.title, req.description, req.price_cents,
        req.item_type, req.item_ref_id, req.cover_media_id)
        .fetch_one(&s.db).await.map_err(internal)?;
    Ok(Json(serde_json::json!({ "listing_id": row.id })))
}

// ---------- 购买 ----------
#[derive(Deserialize)]
pub struct PurchaseReq {
    pub session_token: String,
    pub listing_id: Uuid,
}

pub async fn purchase(
    State(s): State<Arc<AppState>>,
    Json(req): Json<PurchaseReq>,
) -> Result<Json<serde_json::Value>, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;

    // 事务：扣款 + 创建订单 + 商品 purchase_count++
    let mut tx = s.db.begin().await.map_err(internal)?;
    let listing = sqlx::query!(
        r#"SELECT seller_id, price_cents, status FROM market_listings WHERE id = $1 FOR UPDATE"#,
        req.listing_id
    )
    .fetch_optional(&mut *tx)
    .await
    .map_err(internal)?
    .ok_or((StatusCode::NOT_FOUND, "listing not found".into()))?;
    if listing.status != "active" {
        return Err((StatusCode::BAD_REQUEST, "listing not active".into()));
    }
    if listing.seller_id == me {
        return Err((StatusCode::BAD_REQUEST, "cannot buy your own".into()));
    }

    // 余额检查
    let buyer = sqlx::query!(
        "SELECT credit_balance FROM users WHERE id = $1 FOR UPDATE",
        me
    )
    .fetch_one(&mut *tx)
    .await
    .map_err(internal)?;
    if buyer.credit_balance < listing.price_cents {
        return Err((StatusCode::PAYMENT_REQUIRED, "insufficient credit".into()));
    }

    // 扣款 + 卖家加款
    sqlx::query!(
        "UPDATE users SET credit_balance = credit_balance - $1 WHERE id = $2",
        listing.price_cents,
        me
    )
    .execute(&mut *tx)
    .await
    .map_err(internal)?;
    sqlx::query!(
        "UPDATE users SET credit_balance = credit_balance + $1 WHERE id = $2",
        listing.price_cents,
        listing.seller_id
    )
    .execute(&mut *tx)
    .await
    .map_err(internal)?;

    let order = sqlx::query!(
        r#"INSERT INTO market_orders (listing_id, buyer_id, seller_id, price_cents, status, paid_at, delivered_at)
           VALUES ($1, $2, $3, $4, 'delivered', now(), now()) RETURNING id"#,
        req.listing_id, me, listing.seller_id, listing.price_cents)
        .fetch_one(&mut *tx).await.map_err(internal)?;

    // ledger
    sqlx::query!(
        "INSERT INTO credit_ledger (user_id, delta_cents, reason, related_order_id) VALUES ($1, $2, 'purchase', $3)",
        me, -listing.price_cents, order.id).execute(&mut *tx).await.map_err(internal)?;
    sqlx::query!(
        "INSERT INTO credit_ledger (user_id, delta_cents, reason, related_order_id) VALUES ($1, $2, 'sale', $3)",
        listing.seller_id, listing.price_cents, order.id).execute(&mut *tx).await.map_err(internal)?;

    sqlx::query!(
        "UPDATE market_listings SET purchase_count = purchase_count + 1 WHERE id = $1",
        req.listing_id
    )
    .execute(&mut *tx)
    .await
    .map_err(internal)?;

    tx.commit().await.map_err(internal)?;
    Ok(Json(serde_json::json!({
        "order_id": order.id.to_string(),
        "status": "delivered"
    })))
}

// ---------- 我的订单 ----------
#[derive(Deserialize)]
pub struct MyOrdersQ {
    pub session_token: String,
}

#[derive(Serialize)]
pub struct OrderBrief {
    pub id: String,
    pub listing_id: String,
    pub listing_title: String,
    pub price_cents: i64,
    pub status: String,
    pub created_at: i64,
}

pub async fn my_orders(
    State(s): State<Arc<AppState>>,
    Query(q): Query<MyOrdersQ>,
) -> Result<Json<Vec<OrderBrief>>, (StatusCode, String)> {
    let me = auth_user(&s, &q.session_token).await?;
    let rows = sqlx::query!(
        r#"SELECT o.id, o.listing_id, o.price_cents, o.status, o.created_at, l.title
           FROM market_orders o JOIN market_listings l ON l.id = o.listing_id
           WHERE o.buyer_id = $1 ORDER BY o.created_at DESC LIMIT 100"#,
        me
    )
    .fetch_all(&s.db)
    .await
    .map_err(internal)?;
    Ok(Json(
        rows.into_iter()
            .map(|r| OrderBrief {
                id: r.id.to_string(),
                listing_id: r.listing_id.to_string(),
                listing_title: r.title,
                price_cents: r.price_cents,
                status: r.status,
                created_at: r.created_at.timestamp(),
            })
            .collect(),
    ))
}

// ---------- 评价 ----------
#[derive(Deserialize)]
pub struct ReviewReq {
    pub session_token: String,
    pub listing_id: Uuid,
    pub order_id: Option<Uuid>,
    pub rating: i16,
    pub body: Option<String>,
}

pub async fn review(
    State(s): State<Arc<AppState>>,
    Json(req): Json<ReviewReq>,
) -> Result<StatusCode, (StatusCode, String)> {
    let me = auth_user(&s, &req.session_token).await?;
    if !(1..=5).contains(&req.rating) {
        return Err((StatusCode::BAD_REQUEST, "rating 1-5".into()));
    }
    let mut tx = s.db.begin().await.map_err(internal)?;
    let order_id = if let Some(order_id) = req.order_id {
        let ok = sqlx::query_scalar!(
            r#"SELECT 1 as ok FROM market_orders
               WHERE id = $1
                 AND listing_id = $2
                 AND buyer_id = $3
                 AND status = 'delivered'"#,
            order_id,
            req.listing_id,
            me
        )
        .fetch_optional(&mut *tx)
        .await
        .map_err(internal)?
        .is_some();
        if !ok {
            return Err((StatusCode::FORBIDDEN, "order not found for reviewer".into()));
        }
        Some(order_id)
    } else {
        let order_id = sqlx::query_scalar!(
            r#"SELECT id FROM market_orders
               WHERE listing_id = $1
                 AND buyer_id = $2
                 AND status = 'delivered'
               ORDER BY created_at DESC
               LIMIT 1"#,
            req.listing_id,
            me
        )
        .fetch_optional(&mut *tx)
        .await
        .map_err(internal)?;
        let Some(order_id) = order_id else {
            return Err((StatusCode::FORBIDDEN, "review requires purchase".into()));
        };
        Some(order_id)
    };
    sqlx::query!(
        r#"INSERT INTO market_reviews (listing_id, order_id, reviewer_id, rating, body)
           VALUES ($1, $2, $3, $4, $5)
           ON CONFLICT (listing_id, reviewer_id)
           DO UPDATE SET rating = EXCLUDED.rating, body = EXCLUDED.body"#,
        req.listing_id,
        order_id,
        me,
        req.rating,
        req.body
    )
    .execute(&mut *tx)
    .await
    .map_err(internal)?;

    // 重算商品 rating_avg/count
    let agg = sqlx::query!(
        "SELECT COALESCE(AVG(rating::REAL), 0)::REAL as avg, COUNT(*) as cnt FROM market_reviews WHERE listing_id = $1",
        req.listing_id).fetch_one(&mut *tx).await.map_err(internal)?;
    sqlx::query!(
        "UPDATE market_listings SET rating_avg = $1, rating_count = $2 WHERE id = $3",
        agg.avg.unwrap_or(0.0),
        agg.cnt.unwrap_or(0) as i32,
        req.listing_id
    )
    .execute(&mut *tx)
    .await
    .map_err(internal)?;
    tx.commit().await.map_err(internal)?;
    Ok(StatusCode::NO_CONTENT)
}

// ---------- credit 充值 (admin only) ----------
#[derive(Deserialize)]
pub struct GrantReq {
    pub key: Option<String>,
    pub user_id: Uuid,
    pub delta_cents: i64,
    pub reason: Option<String>,
}

pub async fn admin_grant_credit(
    State(s): State<Arc<AppState>>,
    headers: HeaderMap,
    Json(req): Json<GrantReq>,
) -> Result<Json<serde_json::Value>, (StatusCode, String)> {
    let actor = crate::admin_customization::require_actor_or_admin_key(
        &headers,
        &s,
        req.key.as_deref(),
        "admin.users.manage",
    )
    .await?;
    let reason = req.reason.unwrap_or_else(|| "admin_grant".into());
    let mut tx = s.db.begin().await.map_err(internal)?;
    sqlx::query!(
        "UPDATE users SET credit_balance = credit_balance + $1 WHERE id = $2",
        req.delta_cents,
        req.user_id
    )
    .execute(&mut *tx)
    .await
    .map_err(internal)?;
    sqlx::query!(
        "INSERT INTO credit_ledger (user_id, delta_cents, reason) VALUES ($1, $2, $3)",
        req.user_id,
        req.delta_cents,
        &reason
    )
    .execute(&mut *tx)
    .await
    .map_err(internal)?;
    sqlx::query!(
        "INSERT INTO audit_log (actor, action, target, metadata) VALUES ($1, 'admin.grant_credit', $2, $3)",
        &actor.name,
        req.user_id.to_string(),
        serde_json::json!({"delta_cents": req.delta_cents, "reason": reason})
    )
    .execute(&mut *tx)
    .await
    .ok();
    tx.commit().await.map_err(internal)?;
    crate::audit::event(&actor.name, "admin.grant_credit", &req.user_id.to_string());
    Ok(Json(serde_json::json!({"ok": true})))
}
