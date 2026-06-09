#!/usr/bin/env node
import assert from 'node:assert/strict';

const base = (process.env.LAUNCHER_AUDIT_BASE || 'https://127.0.0.1:1337').replace(/\/$/, '');
const runId = process.env.LAUNCHER_AUDIT_RUN_ID || `audit${Date.now().toString(36).slice(-8)}`;
const pass = 'AuditPass123!';
const users = [
  { username: `${runId}_s`, hwid: 'a'.repeat(64) },
  { username: `${runId}_b`, hwid: 'b'.repeat(64) },
  { username: `${runId}_x`, hwid: 'c'.repeat(64) },
];

if (process.env.LAUNCHER_AUDIT_INSECURE_TLS === '1') {
  process.env.NODE_TLS_REJECT_UNAUTHORIZED = '0';
}

function log(label, detail = '') {
  console.log(`PASS ${label}${detail ? ` | ${detail}` : ''}`);
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
  const res = await fetch(url, opts);
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
  const res = await fetch(base + path, { method: 'POST', body: fd });
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

async function main() {
  console.log(`RUN_ID ${runId}`);
  console.log(`BASE ${base}`);

  for (const u of users) {
    u.reg = await req('POST', '/api/auth/register', {
      expected: [200],
      body: { username: u.username, password: pass, email: null, hwid_hex: u.hwid, client_ver: 'audit-smoke', invite_code: null },
    });
    u.token = u.reg.session_token;
    u.user_id = u.reg.user_id;
    u.uid = u.reg.uid;
  }
  log('auth.register', users.map(u => u.username).join(','));

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
  log('auth.login negative paths');

  await req('POST', '/api/heartbeat', { expected: [200], body: { session_token: users[0].token, hwid_hex: users[0].hwid } });
  await req('GET', '/api/subscription', { expected: [200], tokenQuery: users[0].token });
  await req('POST', '/api/profile/update', { expected: [200], body: { session_token: users[0].token, status_text: 'audit online', bio: 'audit bio' } });
  await req('POST', '/api/profile/tags/add', { expected: [200], body: { session_token: users[0].token, tag: 'audit' } });
  await req('POST', '/api/profile/status', { expected: [204], body: { session_token: users[0].token, status: 'busy' } });
  const profile = await req('GET', '/api/profile', { expected: [200], tokenQuery: users[0].token });
  assert.equal(profile.username, users[0].username);
  log('profile/heartbeat/subscription');

  const wsMsgs = await wsOnce(users[0].token, async () => {
    await req('POST', '/api/profile/status', { expected: [204], body: { session_token: users[1].token, status: 'away' } });
  }, ['status']);
  log('websocket status broadcast', wsMsgs.map(m => m.type).join(','));

  const dm = await req('POST', '/api/chat/dm', { expected: [200], body: { session_token: users[0].token, other_user_id: users[1].user_id } });
  const sent = await req('POST', '/api/chat/send', {
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
  log('chat permissions and CRUD');

  const png = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/p9sAAAAASUVORK5CYII=', 'base64');
  const media = await formUpload('/api/media/upload', users[0].token, png, 'text/plain', 'fake.txt', [200]);
  assert.equal(media.mime, 'image/png');
  await formUpload('/api/media/upload', users[0].token, Buffer.from('not an image'), 'image/png', 'fake.png', [415]);
  await formUpload('/api/profile/avatar', users[0].token, Buffer.from('not an image'), 'image/png', 'avatar.png', [415]);
  await req('GET', '/api/media/not-a-sha/file.png', { expected: [400], tokenQuery: users[0].token });
  const dl = await req('GET', `${media.url}?session_token=${encodeURIComponent(users[0].token)}`, { expected: [200], raw: true });
  assert((await dl.arrayBuffer()).byteLength > 0);
  log('media/avatar upload probes');

  const sticker = await req('POST', '/api/sticker', {
    expected: [200],
    body: { session_token: users[0].token, media_id: media.media_id, emoji_alias: ':audit:', label: 'Audit', is_animated: false },
  });
  const pack = await req('POST', '/api/sticker/pack', {
    expected: [200],
    body: { session_token: users[0].token, name: `${runId} pack`, short_name: null, description: 'audit', cover_media_id: media.media_id, is_public: true },
  });
  await req('POST', '/api/sticker/pack/add', { expected: [204], body: { session_token: users[0].token, pack_id: pack.id, sticker_id: sticker.id, sort_order: 1 } });
  await req('POST', '/api/sticker/pack/rename', { expected: [403], body: { session_token: users[1].token, pack_id: pack.id, new_name: 'stolen' } });
  await req('POST', '/api/sticker/delete', { expected: [404], body: { session_token: users[1].token, sha256: media.sha256 } });
  await req('POST', '/api/sticker/pack/remove', { expected: [204], body: { session_token: users[0].token, pack_id: pack.id, sticker_id: sticker.id } });
  await req('POST', '/api/sticker/delete', { expected: [204], body: { session_token: users[0].token, sha256: media.sha256 } });
  log('sticker permissions and CRUD');

  const cats = await req('GET', '/api/market/categories', { expected: [200] });
  const listing = await req('POST', '/api/market/listing/create', {
    expected: [200],
    body: { session_token: users[0].token, category_slug: cats[0].slug, title: `${runId} listing`, description: 'audit', price_cents: 0, item_type: 'other', item_ref_id: null, cover_media_id: media.media_id },
  });
  await req('POST', '/api/market/review', { expected: [403], body: { session_token: users[1].token, listing_id: listing.listing_id, order_id: null, rating: 5, body: 'before purchase' } });
  const purchase = await req('POST', '/api/market/purchase', { expected: [200], body: { session_token: users[1].token, listing_id: listing.listing_id } });
  await req('POST', '/api/market/review', { expected: [204], body: { session_token: users[1].token, listing_id: listing.listing_id, order_id: purchase.order_id, rating: 5, body: 'verified' } });
  log('market purchase/review guard');

  for (const u of users) {
    await req('POST', '/api/auth/logout', { expected: [204], body: { session_token: u.token } });
  }
  log('auth.logout');
  console.log('SMOKE_RESULT ok');
}

main().catch((err) => {
  console.error('SMOKE_RESULT fail');
  console.error(err?.stack || err);
  process.exit(1);
});
