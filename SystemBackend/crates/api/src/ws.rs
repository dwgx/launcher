// WebSocket /ws/chat：用户连接后注册到全局 broadcast hub，发消息时 fanout。
//
// 客户端流程：
//   1. wss://host/ws/chat?session_token=...
//   2. 收到 { "type": "ready" } 表示注册成功
//   3. 之后每条消息推送 { "type": "message", "data": MessageOut }
//
// 服务端：每个连接持有 mpsc receiver；chat::send 时 push_to(uid, json) 入对应 receiver。

use crate::state::AppState;
use crate::media::auth_user;
use axum::{
    extract::{State, ws::{WebSocket, WebSocketUpgrade, Message}, Query},
    response::IntoResponse,
};
use std::sync::{Arc, Mutex};
use std::collections::HashMap;
use serde::Deserialize;
use tokio::sync::mpsc;
use uuid::Uuid;
use once_cell::sync::Lazy;

type Tx = mpsc::UnboundedSender<String>;

// 全局 hub: user_id -> 多个连接（用户可能多设备登录）
static HUB: Lazy<Mutex<HashMap<Uuid, Vec<Tx>>>> = Lazy::new(|| Mutex::new(HashMap::new()));

pub fn push_to(_state: &AppState, user_id: Uuid, payload: &serde_json::Value) {
    let body = match serde_json::to_string(payload) { Ok(s) => s, Err(_) => return };
    let h = HUB.lock().unwrap();
    if let Some(senders) = h.get(&user_id) {
        for s in senders {
            let _ = s.send(body.clone());
        }
    }
}

// 广播给所有在线用户（用于状态变化等全局事件）
pub fn broadcast_all(_state: &AppState, payload: &serde_json::Value) {
    let body = match serde_json::to_string(payload) { Ok(s) => s, Err(_) => return };
    let h = HUB.lock().unwrap();
    for (_uid, senders) in h.iter() {
        for s in senders {
            let _ = s.send(body.clone());
        }
    }
}

#[derive(Deserialize)]
pub struct WsQ { pub session_token: String }

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
    upgrade.on_upgrade(move |socket| handle(socket, uid)).into_response()
}

async fn handle(mut socket: WebSocket, uid: Uuid) {
    let (tx, mut rx) = mpsc::unbounded_channel::<String>();
    {
        let mut h = HUB.lock().unwrap();
        h.entry(uid).or_default().push(tx.clone());
    }
    tracing::info!("ws connected: user={}", uid);
    let _ = socket.send(Message::Text(r#"{"type":"ready"}"#.into())).await;

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
    let mut h = HUB.lock().unwrap();
    if let Some(v) = h.get_mut(&uid) {
        v.retain(|s| !s.same_channel(&tx));
        if v.is_empty() { h.remove(&uid); }
    }
    tracing::info!("ws disconnected: user={}", uid);
}
