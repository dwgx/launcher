#!/usr/bin/env node
import assert from 'node:assert/strict';
import { writeFileSync } from 'node:fs';
import { randomUUID } from 'node:crypto';

const base = (process.env.LAUNCHER_AUDIT_BASE || 'http://127.0.0.1:1338').replace(/\/$/, '');
const runId = (process.env.LAUNCHER_AUDIT_RUN_ID || `chat${Date.now().toString(36)}`)
  .replace(/[^a-zA-Z0-9_.-]/g, '')
  .toLowerCase();
const emitJson = process.env.LAUNCHER_AUDIT_JSON === '1';
const jsonPath = process.env.LAUNCHER_AUDIT_JSON_PATH || '';
const pass = 'ChatSmoke123!';
const steps = [];
const reportStartedAt = nowIso();
const userPrefix = runId.length > 28 ? runId.slice(0, 28) : runId;
const chatUserCount = Math.max(4, Math.min(12, Number.parseInt(process.env.LAUNCHER_CHAT_USERS || '5', 10) || 5));
const adminKey = process.env.LAUNCHER_AUDIT_ADMIN_KEY || '';

const users = Array.from({ length: chatUserCount }, (_, i) => ({
  username: `${userPrefix}_c${String.fromCharCode(97 + i)}`,
  hwid: ((i + 1).toString(16)).repeat(64).slice(0, 64),
}));

if (process.env.LAUNCHER_AUDIT_INSECURE_TLS === '1') {
  process.env.NODE_TLS_REJECT_UNAUTHORIZED = '0';
}

function nowIso() {
  return new Date().toISOString();
}

function isProductionUrl(raw) {
  let url;
  try {
    url = new URL(raw);
  } catch {
    return false;
  }
  const host = url.hostname.toLowerCase();
  return host === '154.40.36.22' || host === 'launcher.dwgx.com';
}

if (isProductionUrl(base)) {
  console.error(`Refusing production chat smoke target: ${base}`);
  process.exit(2);
}

function log(label, detail = '') {
  console.log(`PASS ${label}${detail ? ` | ${detail}` : ''}`);
}

function record(name, status, startedAt, detail = '') {
  steps.push({
    name,
    status,
    started_at: startedAt,
    ended_at: nowIso(),
    detail,
  });
}

async function step(name, fn) {
  const startedAt = nowIso();
  try {
    const detail = await fn();
    record(name, 'pass', startedAt, detail || '');
    log(name, detail || '');
  } catch (err) {
    record(name, 'fail', startedAt, err?.message || String(err));
    throw err;
  }
}

function writeReport(result, error = null) {
  if (!emitJson && !jsonPath) return;
  const report = {
    run_id: runId,
    base,
    result,
    foreground_used: false,
    mouse_used: false,
    topmost_used: false,
    started_at: reportStartedAt,
    ended_at: nowIso(),
    steps,
    error: error ? String(error?.stack || error) : null,
  };
  const text = JSON.stringify(report, null, 2);
  if (jsonPath) {
    writeFileSync(jsonPath, text);
  } else {
    console.log(`SMOKE_JSON ${text}`);
  }
}

async function req(method, path, { body, tokenQuery, expected, raw = false } = {}) {
  let url = base + path;
  if (tokenQuery) {
    url += (url.includes('?') ? '&' : '?') + `session_token=${encodeURIComponent(tokenQuery)}`;
  }
  const opts = { method, headers: {} };
  if (body !== undefined) {
    opts.headers['content-type'] = 'application/json';
    opts.body = JSON.stringify(body);
  }

  let res;
  try {
    res = await fetch(url, opts);
  } catch (err) {
    const cause = err?.cause;
    const parts = [
      `${method} ${path} fetch failed`,
      cause?.code && `code=${cause.code}`,
      cause?.address && `address=${cause.address}`,
      cause?.port && `port=${cause.port}`,
      cause?.message && `cause=${cause.message}`,
    ].filter(Boolean);
    throw new Error(parts.join(' | '), { cause: err });
  }

  if (expected && !expected.includes(res.status)) {
    const text = await res.text().catch(() => '');
    throw new Error(`${method} ${path} expected ${expected.join('/')} got ${res.status}: ${text.slice(0, 500)}`);
  }
  if (raw) return res;
  if (res.status === 204) return null;
  const text = await res.text();
  if (!text) return null;
  try {
    return JSON.parse(text);
  } catch {
    return text;
  }
}

async function formUpload(path, token, bytes, type, filename = 'chat-smoke.png', expected = [200]) {
  const fd = new FormData();
  fd.append('session_token', token);
  fd.append('file', new Blob([bytes], { type }), filename);
  let res;
  try {
    res = await fetch(base + path, { method: 'POST', body: fd });
  } catch (err) {
    const cause = err?.cause;
    const parts = [
      `POST ${path} fetch failed`,
      cause?.code && `code=${cause.code}`,
      cause?.address && `address=${cause.address}`,
      cause?.port && `port=${cause.port}`,
      cause?.message && `cause=${cause.message}`,
    ].filter(Boolean);
    throw new Error(parts.join(' | '), { cause: err });
  }
  if (!expected.includes(res.status)) {
    const text = await res.text().catch(() => '');
    throw new Error(`POST ${path} expected ${expected.join('/')} got ${res.status}: ${text.slice(0, 500)}`);
  }
  if (res.status === 204) return null;
  const text = await res.text();
  if (!text) return null;
  try {
    return JSON.parse(text);
  } catch {
    return text;
  }
}

function wsUrl(token) {
  return base
    .replace(/^http:/, 'ws:')
    .replace(/^https:/, 'wss:')
    + `/ws/chat?session_token=${encodeURIComponent(token)}`;
}

function waitForWs(token, trigger, predicate, timeoutMs = 10000) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(wsUrl(token));
    const seen = [];
    let triggered = false;
    const timer = setTimeout(() => {
      try { ws.close(); } catch {}
      reject(new Error(`WebSocket timeout; seen=${seen.map(m => m.type || m.event_type || 'unknown').join(',')}`));
    }, timeoutMs);

    const finish = (value) => {
      clearTimeout(timer);
      try { ws.close(); } catch {}
      resolve(value);
    };

    ws.onopen = () => {};
    ws.onerror = () => {
      clearTimeout(timer);
      reject(new Error('WebSocket error'));
    };
    ws.onmessage = async (ev) => {
      let msg;
      try {
        msg = JSON.parse(String(ev.data));
      } catch {
        msg = { raw: String(ev.data) };
      }
      seen.push(msg);

      if (msg.type === 'ready' && !triggered) {
        triggered = true;
        try {
          await trigger();
        } catch (err) {
          clearTimeout(timer);
          reject(err);
        }
      }

      if (predicate(msg, seen)) {
        finish({ msg, seen });
      }
    };
  });
}

function messagePayloadText(message) {
  const payload = message?.payload ?? message?.data?.payload;
  if (typeof payload === 'string') return payload;
  if (payload && typeof payload.text === 'string') return payload.text;
  return '';
}

function assertHistoryOrderDesc(messages) {
  for (let i = 1; i < messages.length; i++) {
    assert(messages[i - 1].id > messages[i].id, 'history should be newest-first by message id');
  }
}

function requireChannel(channels, slug) {
  const channel = channels.find(c => c.slug === slug);
  assert(channel, `official channel ${slug} missing`);
  assert(channel.id, `official channel ${slug} id missing`);
  return channel;
}

function assertEvent(events, eventType, messageId, label) {
  const hit = events.find(e => e.event_type === eventType && e.message_id === messageId);
  assert(hit, `${label || eventType} event missing for message ${messageId}`);
  assert(hit.id > 0, `${label || eventType} event id missing`);
  return hit;
}

async function patchUserRole(user, role, roleLabel, isAdmin = true) {
  if (!adminKey) return false;
  await req('POST', `/api/admin/users/${encodeURIComponent(user.user_id)}`, {
    expected: [204],
    body: { key: adminKey, role, role_label: roleLabel, is_admin: isAdmin },
  });
  user.role = role;
  user.role_label = roleLabel;
  user.is_admin = isAdmin;
  return true;
}

async function main() {
  console.log(`RUN_ID ${runId}`);
  console.log(`BASE ${base}`);
  console.log('MODE chat-api');

  await step('auth.register pair', async () => {
    for (const u of users) {
      const reg = await req('POST', '/api/auth/register', {
        expected: [200],
        body: {
          username: u.username,
          password: pass,
          email: null,
          hwid_hex: u.hwid,
          client_ver: 'chat-smoke',
          invite_code: null,
        },
      });
      u.token = reg.session_token;
      u.user_id = reg.user_id;
      u.uid = reg.uid;
    }
    return users.map(u => u.username).join(',');
  });

  await step('admin roles fixture', async () => {
    if (!adminKey) return 'SKIP no LAUNCHER_AUDIT_ADMIN_KEY';
    await patchUserRole(users[0], 'owner', '超级管理员', true);
    await patchUserRole(users[1], 'admin', '管理员', true);
    await patchUserRole(users[3], 'admin', '管理员二号', true);
    await patchUserRole(users[2], 'user', '用户', false);
    return `owner=${users[0].username} admin=${users[1].username} admin2=${users[3].username} normal=${users[2].username}`;
  });

  let official;
  let general;
  let random;
  await step('forum.enter official channels', async () => {
    const perUser = await Promise.all(users.map(u =>
      req('GET', '/api/chat/official', { expected: [200], tokenQuery: u.token })
    ));
    for (const channels of perUser) {
      assert(Array.isArray(channels), 'official channels response must be an array');
      const g = requireChannel(channels, 'general');
      const r = requireChannel(channels, 'random');
      assert.equal(g.write_role, 'user');
      assert.equal(r.write_role, 'user');
    }
    official = perUser[0];
    general = requireChannel(official, 'general');
    random = requireChannel(official, 'random');
    const adminOnly = official.find(c => c.write_role === 'admin_only');
    assert(adminOnly, 'expected at least one admin_only official channel');
    await req('POST', '/api/chat/send', {
      expected: [403],
      body: {
        session_token: users[2].token,
        chat_id: adminOnly.id,
        msg_type: 'text',
        payload: `${runId} forbidden admin channel`,
        client_msg_id: randomUUID(),
        device_id: 'chat-smoke',
      },
    });
    return `users=${users.length} channels=${official.length} general=${general.id} random=${random.id}`;
  });

  const sentOfficial = [];
  await step('chat.general text send', async () => {
    for (const suffix of ['first', 'second']) {
      const text = `${runId} general ${suffix}`;
      const sent = await req('POST', '/api/chat/send', {
        expected: [200],
        body: {
          session_token: users[0].token,
          chat_id: general.id,
          msg_type: 'text',
          payload: text,
          reply_to_id: null,
          client_msg_id: randomUUID(),
          device_id: 'chat-smoke',
        },
      });
      assert.equal(sent.chat_id, general.id);
      assert.equal(sent.msg_type, 'text');
      assert.equal(messagePayloadText(sent), text);
      assert(sent.event_id > 0, 'sent message should include event_id');
      sentOfficial.push(sent);
    }
    assert(sentOfficial[1].id > sentOfficial[0].id, 'later send should have larger id');
    return `ids=${sentOfficial.map(m => m.id).join(',')}`;
  });

  await step('chat.concurrent users general/random', async () => {
    const sends = await Promise.all(users.map((u, i) => {
      const channel = i % 2 === 0 ? general : random;
      const text = `${runId} concurrent ${i} ${channel.slug}`;
      const clientId = randomUUID();
      return req('POST', '/api/chat/send', {
        expected: [200],
        body: {
          session_token: u.token,
          chat_id: channel.id,
          msg_type: 'text',
          payload: text,
          reply_to_id: null,
          client_msg_id: clientId,
          device_id: `chat-smoke-${i}`,
        },
      }).then(sent => ({ sent, text, clientId, channel }));
    }));
    const seenIds = new Set();
    const seenClientIds = new Set();
    for (const { sent, text, clientId, channel } of sends) {
      assert.equal(sent.chat_id, channel.id);
      assert.equal(messagePayloadText(sent), text);
      assert.equal(sent.client_msg_id, clientId);
      assert(sent.event_id > 0, 'concurrent send should include event_id');
      assert(!seenIds.has(sent.id), 'duplicate message id in concurrent sends');
      assert(!seenClientIds.has(sent.client_msg_id), 'duplicate client_msg_id in concurrent sends');
      seenIds.add(sent.id);
      seenClientIds.add(sent.client_msg_id);
      sentOfficial.push(sent);
    }
    return `sent=${sends.length} ids=${[...seenIds].join(',')}`;
  });

  let specialMessage;
  await step('chat.special payload and reply validation', async () => {
    const specialText = `${runId} braces {alpha} } quote "slash" search-anchor`;
    specialMessage = await req('POST', '/api/chat/send', {
      expected: [200],
      body: {
        session_token: users[0].token,
        chat_id: general.id,
        msg_type: 'text',
        payload: specialText,
        reply_to_id: null,
        client_msg_id: randomUUID(),
        device_id: 'chat-smoke-special',
      },
    });
    assert.equal(messagePayloadText(specialMessage), specialText);
    assert.equal(specialMessage.chat_id, general.id);
    sentOfficial.push(specialMessage);

    await req('POST', '/api/chat/send', {
      expected: [400],
      body: {
        session_token: users[1].token,
        chat_id: random.id,
        msg_type: 'text',
        payload: `${runId} invalid cross-channel reply`,
        reply_to_id: specialMessage.id,
        client_msg_id: randomUUID(),
        device_id: 'chat-smoke-cross-reply',
      },
    });
    return `special=${specialMessage.id}`;
  });

  await step('profile.identity fields', async () => {
    const sameDisplayName = `${runId.slice(0, 12)} same`;
    await Promise.all([0, 1].map(i => req('POST', '/api/profile/nickname', {
      expected: [204],
      body: {
        session_token: users[i].token,
        new_nickname: sameDisplayName,
      },
    })));
    await req('POST', '/api/profile/update', {
      expected: [200],
      body: {
        session_token: users[0].token,
        status_text: `${runId} building forum channel`,
        bio: `${runId} smoke bio`,
      },
    });
    await req('POST', '/api/profile/tags/add', {
      expected: [200],
      body: { session_token: users[0].token, tag: 'forum-smoke' },
    });
    const peerByUid = await req('GET', `/api/profile/peer/${encodeURIComponent(users[0].uid)}`, {
      expected: [200],
      tokenQuery: users[1].token,
    });
    assert.equal(peerByUid.username, users[0].username);
    assert.equal(peerByUid.nickname, sameDisplayName);
    assert.equal(peerByUid.status_text, `${runId} building forum channel`);
    assert.equal(peerByUid.bio, `${runId} smoke bio`);
    assert(peerByUid.tags.includes('forum-smoke'), 'peer profile missing tag');
    assert(peerByUid.role, 'peer profile role missing');
    assert(peerByUid.role_label !== undefined, 'peer profile role_label field missing');
    const peerSameName = await req('GET', `/api/profile/peer/${encodeURIComponent(users[1].uid)}`, {
      expected: [200],
      tokenQuery: users[0].token,
    });
    assert.equal(peerSameName.nickname, sameDisplayName);
    assert.notEqual(peerSameName.uid, peerByUid.uid, 'same display names must keep distinct uid');
    const peerByUserId = await req('GET', `/api/profile/peer/${encodeURIComponent(users[0].user_id)}`, {
      expected: [200],
      tokenQuery: users[2].token,
    });
    assert.equal(peerByUserId.uid, users[0].uid);
    return `uid=${peerByUid.uid} role=${peerByUid.role}`;
  });

  await step('chat.general history order', async () => {
    const history = await req(
      'GET',
      `/api/chat/history?chat_id=${encodeURIComponent(general.id)}&limit=20`,
      { expected: [200], tokenQuery: users[1].token },
    );
    assert(Array.isArray(history), 'history response must be an array');
    assertHistoryOrderDesc(history);
    const ids = history.map(m => m.id);
    assert(ids.includes(sentOfficial[0].id), 'history missing first smoke message');
    assert(ids.includes(sentOfficial[1].id), 'history missing second smoke message');
    assert(messagePayloadText(history.find(m => m.id === sentOfficial[1].id)).includes('second'));
    return `history=${history.length}`;
  });

  await step('chat.history pagination and newcomer access', async () => {
    const newer = await req(
      'GET',
      `/api/chat/history?chat_id=${encodeURIComponent(general.id)}&limit=1`,
      { expected: [200], tokenQuery: users[2].token },
    );
    assert.equal(newer.length, 1, 'limit=1 should return one message');
    assert(sentOfficial.some(m => m.id === newer[0].id), 'newest page should contain a smoke message');

    const older = await req(
      'GET',
      `/api/chat/history?chat_id=${encodeURIComponent(general.id)}&before_id=${encodeURIComponent(newer[0].id)}&limit=5`,
      { expected: [200], tokenQuery: users[2].token },
    );
    assert(older.some(m => m.id === sentOfficial[0].id), 'before_id page missing older smoke message');
    assert(older.every(m => m.id < newer[0].id), 'before_id page must only contain older messages');
    const pageIds = new Set([newer[0].id]);
    for (const m of older) {
      assert(!pageIds.has(m.id), 'pagination returned duplicate message id');
      pageIds.add(m.id);
    }

    const list = await req('GET', '/api/chat/list', { expected: [200], tokenQuery: users[2].token });
    assert(Array.isArray(list), 'chat list response must be an array');
    assert(list.some(c => c.id === general.id), 'read state should make official channel visible in chat list');
    return `newest=${newer[0].id} older=${older.length}`;
  });

  let replyMessage;
  await step('chat.reply and idempotency', async () => {
    const replyClientId = randomUUID();
    const replyText = `${runId} reply to first`;
    replyMessage = await req('POST', '/api/chat/send', {
      expected: [200],
      body: {
        session_token: users[1].token,
        chat_id: general.id,
        msg_type: 'text',
        payload: replyText,
        reply_to_id: sentOfficial[0].id,
        mentions: [users[0].user_id],
        client_msg_id: replyClientId,
        device_id: 'chat-smoke-b',
      },
    });
    assert.equal(replyMessage.reply_to_id, sentOfficial[0].id);
    assert(replyMessage.reply_snapshot, 'reply response missing reply_snapshot');
    assert.equal(replyMessage.reply_snapshot.id, sentOfficial[0].id);
    assert(Array.isArray(replyMessage.mentions), 'reply response missing mentions array');
    assert(replyMessage.mentions.some(m => m.user_id === users[0].user_id), 'reply response missing mentioned user');
    assert.equal(messagePayloadText(replyMessage), replyText);
    assert.equal(replyMessage.client_msg_id, replyClientId);

    const duplicate = await req('POST', '/api/chat/send', {
      expected: [200],
      body: {
        session_token: users[1].token,
        chat_id: general.id,
        msg_type: 'text',
        payload: `${runId} should not duplicate`,
        reply_to_id: sentOfficial[0].id,
        mentions: [users[0].user_id],
        client_msg_id: replyClientId,
        device_id: 'chat-smoke-b-retry',
      },
    });
    assert.equal(duplicate.id, replyMessage.id, 'idempotent resend should return the original message');
    assert.equal(messagePayloadText(duplicate), replyText, 'idempotent resend should preserve original payload');

    const history = await req(
      'GET',
      `/api/chat/history?chat_id=${encodeURIComponent(general.id)}&limit=20`,
      { expected: [200], tokenQuery: users[2].token },
    );
    const copies = history.filter(m => m.client_msg_id === replyClientId);
    assert.equal(copies.length, 1, 'history should contain one idempotent reply');
    assert.equal(copies[0].reply_to_id, sentOfficial[0].id);
    assert(copies[0].reply_snapshot, 'history reply missing reply_snapshot');
    assert.equal(copies[0].reply_snapshot.id, sentOfficial[0].id);
    assert(copies[0].mentions.some(m => m.user_id === users[0].user_id), 'history reply missing mention');
    sentOfficial.push(replyMessage);
    return `reply=${replyMessage.id} duplicate=${duplicate.id}`;
  });

  await step('chat.global moderation mute/unmute', async () => {
    if (!adminKey) return 'SKIP no LAUNCHER_AUDIT_ADMIN_KEY';
    const reason = `audit mute ${runId}`;
    const muted = await req('POST', '/api/chat/moderation/mute', {
      expected: [200],
      body: {
        session_token: users[1].token,
        chat_id: general.id,
        target_user_id: users[2].user_id,
        duration_seconds: 120,
        reason,
      },
    });
    assert.equal(muted.active, true, 'target should be actively muted');
    assert.equal(muted.can_unmute, true, 'original moderator should be able to unmute');

    await req('POST', '/api/chat/send', {
      expected: [403],
      body: {
        session_token: users[2].token,
        chat_id: general.id,
        msg_type: 'text',
        payload: `${runId} muted general should fail`,
        client_msg_id: randomUUID(),
        device_id: 'chat-smoke-muted',
      },
    });

    await req('POST', '/api/chat/moderation/unmute', {
      expected: [403],
      body: {
        session_token: users[3].token,
        chat_id: general.id,
        target_user_id: users[2].user_id,
        reason: 'other admin should not unmute',
      },
    });

    const dm = await req('POST', '/api/chat/dm', {
      expected: [200],
      body: { session_token: users[0].token, other_user_id: users[2].user_id },
    });
    await req('POST', '/api/chat/send', {
      expected: [403],
      body: {
        session_token: users[2].token,
        chat_id: dm.id,
        msg_type: 'text',
        payload: `${runId} muted dm should fail`,
        client_msg_id: randomUUID(),
        device_id: 'chat-smoke-muted-dm',
      },
    });

    await req('POST', '/api/chat/moderation/unmute', {
      expected: [200],
      body: {
        session_token: users[0].token,
        chat_id: general.id,
        target_user_id: users[2].user_id,
        reason: 'audit owner unmute',
      },
    });
    const after = await req('POST', '/api/chat/send', {
      expected: [200],
      body: {
        session_token: users[2].token,
        chat_id: general.id,
        msg_type: 'text',
        payload: `${runId} unmuted send ok`,
        client_msg_id: randomUUID(),
        device_id: 'chat-smoke-unmuted',
      },
    });
    const events = await req('GET', `/api/chat/sync?after_event_id=0&limit=300`, {
      expected: [200],
      tokenQuery: users[0].token,
    });
    assert(events.some(e => e.event_type === 'member_muted'), 'member_muted event missing');
    assert(events.some(e => e.event_type === 'member_unmuted'), 'member_unmuted event missing');
    return `mute=${muted.mute_id} after=${after.id}`;
  });

  await step('chat.reaction read delete events', async () => {
    await req('POST', '/api/chat/react', {
      expected: [204],
      body: {
        session_token: users[2].token,
        message_id: replyMessage.id,
        emoji: 'ok',
        remove: false,
      },
    });
    await req('POST', '/api/chat/react', {
      expected: [204],
      body: {
        session_token: users[2].token,
        message_id: replyMessage.id,
        emoji: 'ok',
        remove: true,
      },
    });
    await req('POST', '/api/chat/read', {
      expected: [204],
      body: {
        session_token: users[2].token,
        chat_id: general.id,
        up_to_message_id: replyMessage.id,
      },
    });
    await req('POST', '/api/chat/delete', {
      expected: [403],
      body: { session_token: users[0].token, message_id: replyMessage.id },
    });
    await req('POST', '/api/chat/delete', {
      expected: [204],
      body: { session_token: users[1].token, message_id: replyMessage.id },
    });
    await req('POST', '/api/chat/react', {
      expected: [404],
      body: {
        session_token: users[2].token,
        message_id: replyMessage.id,
        emoji: 'late',
        remove: false,
      },
    });
    await req('POST', '/api/chat/read', {
      expected: [404],
      body: {
        session_token: users[2].token,
        chat_id: general.id,
        up_to_message_id: replyMessage.id,
      },
    });
    await req('POST', '/api/chat/send', {
      expected: [400],
      body: {
        session_token: users[2].token,
        chat_id: general.id,
        msg_type: 'text',
        payload: `${runId} invalid deleted reply target`,
        reply_to_id: replyMessage.id,
        client_msg_id: randomUUID(),
        device_id: 'chat-smoke-deleted-reply',
      },
    });
    const history = await req(
      'GET',
      `/api/chat/history?chat_id=${encodeURIComponent(general.id)}&limit=30`,
      { expected: [200], tokenQuery: users[2].token },
    );
    assert(!history.some(m => m.id === replyMessage.id), 'deleted message should be hidden from history');
    const sync = await req(
      'GET',
      `/api/chat/sync?after_event_id=${Math.max(0, sentOfficial[0].event_id - 1)}&limit=100`,
      { expected: [200], tokenQuery: users[2].token },
    );
    assertEvent(sync, 'reaction', replyMessage.id, 'reaction');
    assertEvent(sync, 'read', replyMessage.id, 'read');
    assertEvent(sync, 'message_deleted', replyMessage.id, 'delete');
    return `events=${sync.length}`;
  });

  await step('ws.message general broadcast', async () => {
    const text = `${runId} ws message`;
    const { seen } = await waitForWs(
      users[1].token,
      async () => {
        const sent = await req('POST', '/api/chat/send', {
          expected: [200],
          body: {
            session_token: users[0].token,
            chat_id: general.id,
            msg_type: 'text',
            payload: text,
            reply_to_id: null,
            client_msg_id: randomUUID(),
            device_id: 'chat-smoke',
          },
        });
        sentOfficial.push(sent);
      },
      (msg) => msg.type === 'event'
        && msg.event_type === 'message'
        && msg.chat_id === general.id
        && messagePayloadText(msg.data) === text,
    );
    return seen.map(m => m.type || m.event_type).join(',');
  });

  await step('ws.status broadcast', async () => {
    const { seen } = await waitForWs(
      users[1].token,
      async () => {
        await req('POST', '/api/profile/status', {
          expected: [204],
          body: { session_token: users[0].token, status: 'away' },
        });
      },
      (msg) => msg.type === 'status'
        && msg.user_id === users[0].user_id
        && msg.status === 'away',
    );
    return seen.map(m => m.type || m.event_type).join(',');
  });

  let media;
  let sticker;
  let pack;
  await step('media.sticker seed', async () => {
    const png = Buffer.from(
      'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/p9sAAAAASUVORK5CYII=',
      'base64',
    );
    media = await formUpload('/api/media/upload', users[0].token, png, 'image/png', `${runId}.png`, [200]);
    assert.equal(media.mime, 'image/png');
    assert(media.media_id > 0);

    sticker = await req('POST', '/api/sticker', {
      expected: [200],
      body: {
        session_token: users[0].token,
        media_id: media.media_id,
        emoji_alias: ':chat_smoke:',
        label: 'Chat Smoke',
        is_animated: false,
      },
    });
    assert(sticker.id, 'sticker id missing');

    pack = await req('POST', '/api/sticker/pack', {
      expected: [200],
      body: {
        session_token: users[0].token,
        name: `${runId} pack`,
        short_name: null,
        description: 'chat smoke',
        cover_media_id: media.media_id,
        is_public: true,
      },
    });
    assert(pack.id, 'pack id missing');

    await req('POST', '/api/sticker/pack/add', {
      expected: [204],
      body: {
        session_token: users[0].token,
        pack_id: pack.id,
        sticker_id: sticker.id,
        sort_order: 1,
      },
    });

    const mine = await req('GET', '/api/sticker/mine', { expected: [200], tokenQuery: users[0].token });
    assert(mine.some(s => s.id === sticker.id), 'my stickers missing seeded sticker');
    const packs = await req('GET', '/api/sticker/packs/mine', { expected: [200], tokenQuery: users[0].token });
    assert(packs.some(p => p.id === pack.id), 'my packs missing seeded pack');
    return `media=${media.media_id} sticker=${sticker.id} pack=${pack.id}`;
  });

  await step('chat.sticker send/history', async () => {
    const sent = await req('POST', '/api/chat/send', {
      expected: [200],
      body: {
        session_token: users[0].token,
        chat_id: general.id,
        msg_type: 'sticker',
        payload: {
          sticker_id: sticker.id,
          sticker_pack_id: pack.id,
          media_id: media.media_id,
          media_url: sticker.media_url,
          emoji_alias: sticker.emoji_alias,
        },
        reply_to_id: null,
        client_msg_id: randomUUID(),
        device_id: 'chat-smoke',
      },
    });
    assert.equal(sent.msg_type, 'sticker');
    const history = await req(
      'GET',
      `/api/chat/history?chat_id=${encodeURIComponent(general.id)}&limit=30`,
      { expected: [200], tokenQuery: users[0].token },
    );
    assert(history.some(m => m.id === sent.id && m.msg_type === 'sticker'), 'history missing sticker message');
    return `message=${sent.id}`;
  });

  await step('chat.sync/search', async () => {
    const sync = await req(
      'GET',
      `/api/chat/sync?after_event_id=${Math.max(0, sentOfficial[0].event_id - 1)}&limit=120`,
      { expected: [200], tokenQuery: users[1].token },
    );
    assert(sync.some(e => e.message_id === sentOfficial[0].id), 'sync missing first official message event');
    assert(sync.some(e => e.event_type === 'message' && e.message_id === sentOfficial[1].id), 'sync missing second official message event');

    const search = await req(
      'GET',
      `/api/chat/search?q=${encodeURIComponent(runId)}&limit=20`,
      { expected: [200], tokenQuery: users[1].token },
    );
    assert(search.some(hit => hit.chat_slug === 'general' && String(hit.payload).includes(runId)), 'search missing smoke text');
    assert(search.some(hit => hit.chat_slug === 'general' && hit.id === sentOfficial[0].id), 'search missing first text message');
    assert(search.some(hit => hit.chat_slug === 'general'
      && hit.id === specialMessage.id
      && String(hit.payload).includes('{alpha} } quote "slash"')), 'search should preserve special text payload');
    assert(!search.some(hit => hit.id === replyMessage.id), 'search should not return deleted reply');
    return `sync=${sync.length} search=${search.length}`;
  });

  await step('auth.logout pair', async () => {
    for (const u of users) {
      if (u.token) {
        await req('POST', '/api/auth/logout', { expected: [204], body: { session_token: u.token } });
      }
    }
  });
}

try {
  await main();
  console.log('SMOKE_RESULT ok');
  writeReport('ok');
} catch (err) {
  console.error('SMOKE_RESULT fail');
  console.error(err?.stack || err);
  writeReport('fail', err);
  process.exit(1);
}
