#!/usr/bin/env node
import assert from 'node:assert/strict';
import { writeFileSync } from 'node:fs';

const base = (process.env.LAUNCHER_AUDIT_BASE || 'https://127.0.0.1:1337').replace(/\/$/, '');
const runId = process.env.LAUNCHER_AUDIT_RUN_ID || `audit${Date.now().toString(36).slice(-8)}`;
const mode = (process.env.LAUNCHER_AUDIT_MODE || 'write').toLowerCase();
const emitJson = process.env.LAUNCHER_AUDIT_JSON === '1';
const jsonPath = process.env.LAUNCHER_AUDIT_JSON_PATH || '';
const pass = 'AuditPass123!';
const steps = [];
const users = [
  { username: `${runId}_s`, hwid: 'a'.repeat(64) },
  { username: `${runId}_b`, hwid: 'b'.repeat(64) },
  { username: `${runId}_x`, hwid: 'c'.repeat(64) },
];

if (process.env.LAUNCHER_AUDIT_INSECURE_TLS === '1') {
  process.env.NODE_TLS_REJECT_UNAUTHORIZED = '0';
}

function nowIso() {
  return new Date().toISOString();
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
    mode,
    result,
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
    url += (url.includes('?') ? '&' : '?') + 'session_token=' + encodeURIComponent(tokenQuery);
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
  try { return JSON.parse(text); } catch { return text; }
}

async function formUpload(path, token, bytes, type, filename = 'audit.bin', expected = [200]) {
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
  try { return JSON.parse(text); } catch { return text; }
}

function wsOnce(token, trigger, wantedTypes = []) {
  return new Promise((resolve, reject) => {
    const wsUrl = base.replace(/^http:/, 'ws:').replace(/^https:/, 'wss:') + `/ws/chat?session_token=${encodeURIComponent(token)}`;
    const ws = new WebSocket(wsUrl);
    const timer = setTimeout(() => {
      try { ws.close(); } catch {}
      reject(new Error('WebSocket timeout'));
    }, 10000);
    const seen = [];
    ws.onopen = async () => {
      try { if (trigger) await trigger(); } catch (e) { clearTimeout(timer); reject(e); }
    };
    ws.onerror = () => {
      clearTimeout(timer);
      reject(new Error('WebSocket error'));
    };
    ws.onmessage = (ev) => {
      let msg;
      try { msg = JSON.parse(String(ev.data)); } catch { msg = { raw: String(ev.data) }; }
      seen.push(msg);
      const types = new Set(seen.map(m => m.type));
      if (types.has('ready') && wantedTypes.every(t => types.has(t))) {
        clearTimeout(timer);
        try { ws.close(); } catch {}
        resolve(seen);
      }
    };
  });
}

async function readonlySmoke() {
  console.log(`RUN_ID ${runId}`);
  console.log(`BASE ${base}`);
  console.log('MODE readonly');

  await step('http.market.categories', async () => {
    const cats = await req('GET', '/api/market/categories', { expected: [200] });
    assert(Array.isArray(cats));
    return `count=${cats.length}`;
  });

  await step('http.market.listings', async () => {
    const listings = await req('GET', '/api/market/listings?limit=5', { expected: [200] });
    assert(Array.isArray(listings));
    return `count=${listings.length}`;
  });

  await step('http.admin.reachable', async () => {
    const res = await req('GET', '/admin', { expected: [200, 303, 307, 308], raw: true });
    return `status=${res.status}`;
  });
}

async function writeSmoke() {
  console.log(`RUN_ID ${runId}`);
  console.log(`BASE ${base}`);
  console.log('MODE write');

  await step('auth.register', async () => {
    for (const u of users) {
      u.reg = await req('POST', '/api/auth/register', {
        expected: [200],
        body: { username: u.username, password: pass, email: null, hwid_hex: u.hwid, client_ver: 'audit-smoke', invite_code: null },
      });
      u.token = u.reg.session_token;
      u.user_id = u.reg.user_id;
      u.uid = u.reg.uid;
    }
    return users.map(u => u.username).join(',');
  });

  await step('auth.login negative paths', async () => {
    await req('POST', '/api/auth/login', {
      expected: [200],
      body: { username: users[0].username, password: pass, hwid_hex: users[0].hwid, client_ver: 'audit-smoke' },
    });
    await req('POST', '/api/auth/login', {
      expected: [401],
      body: { username: users[0].username, password: 'wrong-pass', hwid_hex: users[0].hwid, client_ver: 'audit-smoke' },
    });
    await req('POST', '/api/auth/login', {
      expected: [403],
      body: { username: users[0].username, password: pass, hwid_hex: users[1].hwid, client_ver: 'audit-smoke' },
    });
  });

  await step('profile/heartbeat/subscription', async () => {
    await req('POST', '/api/heartbeat', { expected: [200], body: { session_token: users[0].token, hwid_hex: users[0].hwid } });
    await req('POST', '/api/heartbeat', { expected: [401], body: { session_token: 'not-a-token', hwid_hex: users[0].hwid } });
    await req('POST', '/api/heartbeat', { expected: [403], body: { session_token: users[0].token, hwid_hex: users[1].hwid } });
    await req('GET', '/api/subscription', { expected: [200], tokenQuery: users[0].token });
    await req('POST', '/api/profile/update', { expected: [200], body: { session_token: users[0].token, status_text: 'audit online', bio: 'audit bio' } });
    await req('POST', '/api/profile/tags/add', { expected: [200], body: { session_token: users[0].token, tag: 'audit' } });
    await req('POST', '/api/profile/status', { expected: [204], body: { session_token: users[0].token, status: 'busy' } });
    const profile = await req('GET', '/api/profile', { expected: [200], tokenQuery: users[0].token });
    assert.equal(profile.username, users[0].username);
  });

  await step('websocket status broadcast', async () => {
    const wsMsgs = await wsOnce(users[0].token, async () => {
      await req('POST', '/api/profile/status', { expected: [204], body: { session_token: users[1].token, status: 'away' } });
    }, ['status']);
    return wsMsgs.map(m => m.type).join(',');
  });

  let dm;
  let sent;
  await step('chat permissions and CRUD', async () => {
    dm = await req('POST', '/api/chat/dm', { expected: [200], body: { session_token: users[0].token, other_user_id: users[1].user_id } });
    sent = await req('POST', '/api/chat/send', {
      expected: [200],
      body: { session_token: users[0].token, chat_id: dm.id, msg_type: 'text', payload: { text: `hello ${runId}` }, reply_to_id: null },
    });
    await req('GET', `/api/chat/history?chat_id=${encodeURIComponent(dm.id)}&limit=10`, { expected: [200], tokenQuery: users[0].token });
    await req('GET', `/api/chat/history?chat_id=${encodeURIComponent(dm.id)}&limit=10`, { expected: [403], tokenQuery: users[2].token });
    await req('POST', '/api/chat/react', { expected: [204], body: { session_token: users[1].token, message_id: sent.id, emoji: 'ok', remove: false } });
    await req('POST', '/api/chat/react', { expected: [403], body: { session_token: users[2].token, message_id: sent.id, emoji: 'bad', remove: false } });
    await req('POST', '/api/chat/read', { expected: [204], body: { session_token: users[1].token, chat_id: dm.id, up_to_message_id: sent.id } });
    await req('POST', '/api/chat/read', { expected: [403], body: { session_token: users[2].token, chat_id: dm.id, up_to_message_id: sent.id } });
    await req('POST', '/api/chat/delete', { expected: [403], body: { session_token: users[2].token, message_id: sent.id } });
    await req('POST', '/api/chat/delete', { expected: [204], body: { session_token: users[0].token, message_id: sent.id } });
  });

  let media;
  await step('media/avatar upload probes', async () => {
    const png = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/p9sAAAAASUVORK5CYII=', 'base64');
    media = await formUpload('/api/media/upload', users[0].token, png, 'text/plain', 'fake.txt', [200]);
    assert.equal(media.mime, 'image/png');
    await formUpload('/api/media/upload', users[0].token, Buffer.from('not an image'), 'image/png', 'fake.png', [415]);
    await formUpload('/api/profile/avatar', users[0].token, Buffer.from('not an image'), 'image/png', 'avatar.png', [415]);
    await req('GET', '/api/media/not-a-sha/file.png', { expected: [400], tokenQuery: users[0].token });
    await req('GET', media.url, { expected: [401], raw: true });
    const dl = await req('GET', `${media.url}?session_token=${encodeURIComponent(users[0].token)}`, { expected: [200], raw: true });
    assert((await dl.arrayBuffer()).byteLength > 0);
  });

  await step('sticker permissions and CRUD', async () => {
    const sticker = await req('POST', '/api/sticker', {
      expected: [200],
      body: { session_token: users[0].token, media_id: media.media_id, emoji_alias: ':audit:', label: 'Audit', is_animated: false },
    });
    const pack = await req('POST', '/api/sticker/pack', {
      expected: [200],
      body: { session_token: users[0].token, name: `${runId} pack`, short_name: null, description: 'audit', cover_media_id: media.media_id, is_public: true },
    });
    await req('POST', '/api/sticker/pack/add', { expected: [204], body: { session_token: users[0].token, pack_id: pack.id, sticker_id: sticker.id, sort_order: 1 } });
    await req('GET', `/api/sticker/pack/${encodeURIComponent(pack.id)}`, { expected: [200] });
    await req('POST', '/api/sticker/pack/rename', { expected: [403], body: { session_token: users[1].token, pack_id: pack.id, new_name: 'stolen' } });
    await req('POST', '/api/sticker/delete', { expected: [404], body: { session_token: users[1].token, sha256: media.sha256 } });
    await req('POST', '/api/sticker/pack/remove', { expected: [204], body: { session_token: users[0].token, pack_id: pack.id, sticker_id: sticker.id } });
    await req('POST', '/api/sticker/delete', { expected: [204], body: { session_token: users[0].token, sha256: media.sha256 } });
  });

  await step('market purchase/review guard', async () => {
    const cats = await req('GET', '/api/market/categories', { expected: [200] });
    assert(cats.length > 0, 'market categories must not be empty');
    const listing = await req('POST', '/api/market/listing/create', {
      expected: [200],
      body: { session_token: users[0].token, category_slug: cats[0].slug, title: `${runId} listing`, description: 'audit', price_cents: 0, item_type: 'other', item_ref_id: null, cover_media_id: media.media_id },
    });
    await req('POST', '/api/market/review', { expected: [403], body: { session_token: users[1].token, listing_id: listing.listing_id, order_id: null, rating: 5, body: 'before purchase' } });
    const purchase = await req('POST', '/api/market/purchase', { expected: [200], body: { session_token: users[1].token, listing_id: listing.listing_id } });
    await req('POST', '/api/market/review', { expected: [204], body: { session_token: users[1].token, listing_id: listing.listing_id, order_id: purchase.order_id, rating: 5, body: 'verified' } });
  });

  await step('auth.logout', async () => {
    for (const u of users) {
      if (u.token) {
        await req('POST', '/api/auth/logout', { expected: [204, 404], body: { session_token: u.token } });
      }
    }
  });
}

const reportStartedAt = nowIso();

try {
  if (mode === 'readonly') {
    await readonlySmoke();
  } else if (mode === 'write') {
    await writeSmoke();
  } else {
    throw new Error(`unsupported LAUNCHER_AUDIT_MODE: ${mode}`);
  }
  console.log('SMOKE_RESULT ok');
  writeReport('ok');
} catch (err) {
  console.error('SMOKE_RESULT fail');
  console.error(err?.stack || err);
  writeReport('fail', err);
  process.exit(1);
}
