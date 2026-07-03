// WebSocket /ws/chat：用户连接后注册到全局 broadcast hub，发消息时 fanout。
//
// 客户端流程：
//   1. wss://host/ws/chat?session_token=...
//   2. 收到 { "type": "ready" } 表示注册成功
//   3. 之后每条消息推送 { "type": "message", "data": MessageOut }
//
// 服务端：每个连接持有 mpsc receiver；chat::send 时 push_to(uid, json) 入对应 receiver。

use crate::media::auth_user;
use crate::state::AppState;
use axum::{
    extract::{
        ws::{Message, WebSocket, WebSocketUpgrade},
        Query, State,
    },
    response::IntoResponse,
};
use once_cell::sync::Lazy;
use serde::Deserialize;
use std::collections::HashMap;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use tokio::sync::mpsc;
use uuid::Uuid;

type Tx = mpsc::Sender<String>;

/// Each connection is identified by a unique ID (for cleanup without same_channel).
type ConnEntry = (u64, Tx);

static NEXT_CONN_ID: AtomicU64 = AtomicU64::new(0);

/// Max simultaneous WS connections per user.
const MAX_CONNS_PER_USER: usize = 5;

// 全局 hub: user_id -> 多个连接（用户可能多设备登录）
static HUB: Lazy<Mutex<HashMap<Uuid, Vec<ConnEntry>>>> = Lazy::new(|| Mutex::new(HashMap::new()));

// 当前在线用户 id 快照。用于 ticket/forum 类事件的定向扇出：给离线用户 push 是空操作，
// 所以只需对在线用户做可见性判定，避免每条消息全表扫描 users。
pub fn online_user_ids() -> Vec<Uuid> {
    HUB.lock().unwrap_or_else(|e| e.into_inner()).keys().copied().collect()
}

pub fn push_to(_state: &AppState, user_id: Uuid, payload: &serde_json::Value) {
    let body = match serde_json::to_string(payload) {
        Ok(s) => s,
        Err(_) => return,
    };
    let mut h = HUB.lock().unwrap_or_else(|e| e.into_inner());
    if let Some(senders) = h.get_mut(&user_id) {
        // Remove connections whose buffer is full (slow consumer).
        senders.retain(|(_id, s)| s.try_send(body.clone()).is_ok());
        if senders.is_empty() {
            h.remove(&user_id);
        }
    }
}

// 广播给所有在线用户（用于状态变化等全局事件）
pub fn broadcast_all(_state: &AppState, payload: &serde_json::Value) {
    let body = match serde_json::to_string(payload) {
        Ok(s) => s,
        Err(_) => return,
    };
    let mut h = HUB.lock().unwrap_or_else(|e| e.into_inner());
    // Collect user_ids to remove after iteration.
    let mut empty_uids = Vec::new();
    for (uid, senders) in h.iter_mut() {
        senders.retain(|(_id, s)| s.try_send(body.clone()).is_ok());
        if senders.is_empty() {
            empty_uids.push(*uid);
        }
    }
    for uid in empty_uids {
        h.remove(&uid);
    }
}

#[derive(Deserialize)]
pub struct WsQ {
    pub session_token: String,
}

pub async fn ws_handler(
    State(s): State<Arc<AppState>>,
    Query(q): Query<WsQ>,
    upgrade: WebSocketUpgrade,
) -> impl IntoResponse {
    // 鉴权放在 upgrade 之前，失败直接 401
    let uid = match auth_user(&s, &q.session_token).await {
        Ok(u) => u,
        Err((code, msg)) => return (code, msg).into_response(),
    };
    upgrade
        .on_upgrade(move |socket| handle(socket, uid))
        .into_response()
}

async fn handle(mut socket: WebSocket, uid: Uuid) {
    let (tx, mut rx) = mpsc::channel::<String>(512);
    let conn_id = NEXT_CONN_ID.fetch_add(1, Ordering::Relaxed);
    {
        let mut h = HUB.lock().unwrap_or_else(|e| e.into_inner());
        let conns = h.entry(uid).or_default();
        // M-6: enforce per-user connection limit; evict oldest if at cap.
        while conns.len() >= MAX_CONNS_PER_USER {
            conns.remove(0); // oldest connection — its rx will see channel closed
        }
        conns.push((conn_id, tx.clone()));
    }
    tracing::info!("ws connected: user={}", uid);
    let _ = socket
        .send(Message::Text(r#"{"type":"ready"}"#.into()))
        .await;

    // 读 + 写并行
    loop {
        tokio::select! {
            // 服务端推
            push = rx.recv() => {
                match push {
                    Some(body) => {
                        if socket.send(Message::Text(body)).await.is_err() { break; }
                    }
                    None => break,
                }
            }
            // 客户端发（ping / typing 之类）
            in_msg = socket.recv() => {
                match in_msg {
                    Some(Ok(Message::Ping(p))) => { let _ = socket.send(Message::Pong(p)).await; }
                    Some(Ok(Message::Close(_))) | None => break,
                    Some(Err(_)) => break,
                    _ => {}
                }
            }
        }
    }

    // 移除自己
    let mut h = HUB.lock().unwrap_or_else(|e| e.into_inner());
    if let Some(v) = h.get_mut(&uid) {
        v.retain(|(id, _s)| *id != conn_id);
        if v.is_empty() {
            h.remove(&uid);
        }
    }
    tracing::info!("ws disconnected: user={}", uid);
}
