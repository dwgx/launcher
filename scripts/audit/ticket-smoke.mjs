#!/usr/bin/env node
// 工单系统冒烟测试：分类列举 → 建单（私有/公开）→ 列我的/公开/管理员视图 →
// 详情 → 用户回复 → 管理员回复 → 校验消息时序。
// 复用 chat-smoke 的同款 register/req/step 模式，输出同结构 SMOKE_JSON 报告。
import assert from 'node:assert/strict';
import { writeFileSync } from 'node:fs';

const base = (process.env.LAUNCHER_AUDIT_BASE || 'http://127.0.0.1:1338').replace(/\/$/, '');
const runId = (process.env.LAUNCHER_AUDIT_RUN_ID || `ticket${Date.now().toString(36)}`)
  .replace(/[^a-zA-Z0-9_.-]/g, '')
  .toLowerCase();
const emitJson = process.env.LAUNCHER_AUDIT_JSON === '1';
const jsonPath = process.env.LAUNCHER_AUDIT_JSON_PATH || '';
const pass = 'TicketSmoke123!';
const steps = [];
const reportStartedAt = nowIso();
const userPrefix = runId.length > 26 ? runId.slice(0, 26) : runId;
const adminKey = process.env.LAUNCHER_AUDIT_ADMIN_KEY || '';
// 生产开 require_invite_code 时，注册需带有效邀请码（max_uses 要够覆盖用户数）。
const inviteCode = process.env.LAUNCHER_AUDIT_INVITE_CODE || '';

// reporter = 建单普通用户，admin = 处理工单的管理员
const users = [
  { username: `${userPrefix}_tr`, hwid: 'a'.repeat(64), role: 'user' },
  { username: `${userPrefix}_ta`, hwid: 'b'.repeat(64), role: 'admin' },
];
const reporter = users[0];
const admin = users[1];

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
  const prodHosts = (process.env.LAUNCHER_AUDIT_PROD_HOSTS || '')
    .split(',')
    .map(h => h.trim().toLowerCase())
    .filter(Boolean);
  return prodHosts.includes(host);
}

if (isProductionUrl(base)) {
  console.error(`Refusing production ticket smoke target: ${base}`);
  process.exit(2);
}

function log(label, detail = '') {
  console.log(`PASS ${label}${detail ? ` | ${detail}` : ''}`);
}

function record(name, status, startedAt, detail = '') {
  steps.push({ name, status, started_at: startedAt, ended_at: nowIso(), detail });
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
    started_at: reportStartedAt,
    ended_at: nowIso(),
    steps,
    error: error ? String(error?.stack || error) : null,
  };
  const text = JSON.stringify(report, null, 2);
  if (jsonPath) writeFileSync(jsonPath, text);
  else console.log(`SMOKE_JSON ${text}`);
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

async function patchUserRole(user, role, roleLabel, isAdmin) {
  if (!adminKey) return false;
  await req('POST', `/api/admin/users/${encodeURIComponent(user.user_id)}`, {
    expected: [204],
    body: { key: adminKey, role, role_label: roleLabel, is_admin: isAdmin },
  });
  user.role = role;
  user.is_admin = isAdmin;
  return true;
}

async function main() {
  console.log(`RUN_ID ${runId}`);
  console.log(`BASE ${base}`);
  console.log('MODE ticket-api');

  await step('auth.register reporter+admin', async () => {
    for (const u of users) {
      const reg = await req('POST', '/api/auth/register', {
        expected: [200],
        body: {
          username: u.username,
          password: pass,
          email: null,
          hwid_hex: u.hwid,
          client_ver: 'ticket-smoke',
          invite_code: inviteCode || null,
        },
      });
      u.token = reg.session_token;
      u.user_id = reg.user_id;
      u.uid = reg.uid;
    }
    return users.map(u => u.username).join(',');
  });

  await step('admin role fixture', async () => {
    if (!adminKey) return 'SKIP no LAUNCHER_AUDIT_ADMIN_KEY';
    await patchUserRole(admin, 'admin', '管理员', true);
    await patchUserRole(reporter, 'user', '用户', false);
    return `admin=${admin.username} reporter=${reporter.username}`;
  });

  let category;
  await step('ticket.categories list (enabled only for user)', async () => {
    const cats = await req('GET', '/api/ticket/categories', {
      expected: [200],
      tokenQuery: reporter.token,
    });
    assert(Array.isArray(cats) && cats.length > 0, 'expected seeded ticket categories');
    for (const c of cats) {
      assert.equal(c.is_enabled, true, 'user-facing category list must be enabled-only');
      assert(c.id && c.slug && c.name, 'category fields present');
    }
    category = cats[0];
    return `${cats.length} categories, using ${category.slug}`;
  });

  let privateTicket;
  await step('ticket.create private', async () => {
    privateTicket = await req('POST', '/api/tickets', {
      expected: [200],
      body: {
        session_token: reporter.token,
        category_id: category.id,
        title: `${runId} 私有工单`,
        body: '私有工单正文：请协助处理 HWID 绑定。',
        visibility: 'private',
      },
    });
    assert(privateTicket.id, 'ticket id present');
    assert(privateTicket.number > 0, 'ticket number assigned');
    assert.equal(privateTicket.status, 'open', 'new ticket status open');
    assert.equal(privateTicket.visibility, 'private');
    assert.equal(privateTicket.category_id, category.id);
    return `#${privateTicket.number} ${privateTicket.id}`;
  });

  let publicTicket;
  await step('ticket.create public', async () => {
    publicTicket = await req('POST', '/api/tickets', {
      expected: [200],
      body: {
        session_token: reporter.token,
        category_id: category.id,
        title: `${runId} 公开工单`,
        body: '公开工单正文：功能反馈一条。',
        visibility: 'public',
      },
    });
    assert.equal(publicTicket.visibility, 'public');
    return `#${publicTicket.number}`;
  });

  await step('ticket.create rejects empty title/body', async () => {
    await req('POST', '/api/tickets', {
      expected: [400],
      body: {
        session_token: reporter.token,
        category_id: category.id,
        title: '   ',
        body: '   ',
        visibility: 'private',
      },
    });
    return 'empty title/body → 400';
  });

  await step('ticket.list mine includes both', async () => {
    const mine = await req('GET', '/api/tickets/mine', {
      expected: [200],
      tokenQuery: reporter.token,
    });
    assert(Array.isArray(mine), 'mine is array');
    const ids = new Set(mine.map(t => t.id));
    assert(ids.has(privateTicket.id), 'mine includes private ticket');
    assert(ids.has(publicTicket.id), 'mine includes public ticket');
    return `${mine.length} of mine`;
  });

  await step('ticket.list public shows public, hides private', async () => {
    const pub = await req('GET', '/api/tickets/public', {
      expected: [200],
      tokenQuery: admin.token,
    });
    const ids = new Set(pub.map(t => t.id));
    assert(ids.has(publicTicket.id), 'public list includes public ticket');
    assert(!ids.has(privateTicket.id), 'public list must hide private ticket');
    return `${pub.length} public`;
  });

  await step('ticket.list admin sees private (if admin role applied)', async () => {
    if (!adminKey) return 'SKIP no admin role';
    const all = await req('GET', '/api/admin/tickets', {
      expected: [200],
      tokenQuery: admin.token,
    });
    const ids = new Set(all.map(t => t.id));
    assert(ids.has(privateTicket.id), 'admin sees private ticket');
    assert(ids.has(publicTicket.id), 'admin sees public ticket');
    return `${all.length} visible to admin`;
  });

  await step('ticket.detail has opening message', async () => {
    const detail = await req('GET', `/api/tickets/${privateTicket.id}`, {
      expected: [200],
      tokenQuery: reporter.token,
    });
    assert(detail.ticket && detail.ticket.id === privateTicket.id, 'detail ticket matches');
    assert(Array.isArray(detail.messages) && detail.messages.length >= 1, 'has opening message');
    const first = detail.messages[0];
    const text = typeof first.payload === 'string' ? first.payload : first.payload?.text;
    assert(String(text).includes('HWID'), 'opening message body present');
    return `${detail.messages.length} messages`;
  });

  await step('ticket.reply by reporter via chat/send', async () => {
    const sent = await req('POST', '/api/chat/send', {
      expected: [200],
      body: {
        session_token: reporter.token,
        chat_id: privateTicket.chat_id,
        msg_type: 'text',
        payload: { text: `${runId} 用户补充信息` },
      },
    });
    assert(sent.id > 0, 'reply message id');
    return `msg ${sent.id}`;
  });

  await step('ticket.reply by admin then verify ordering', async () => {
    if (!adminKey) return 'SKIP no admin role';
    const sent = await req('POST', '/api/chat/send', {
      expected: [200],
      body: {
        session_token: admin.token,
        chat_id: privateTicket.chat_id,
        msg_type: 'text',
        payload: { text: `${runId} 管理员已处理` },
      },
    });
    assert(sent.id > 0, 'admin reply id');
    const detail = await req('GET', `/api/tickets/${privateTicket.id}`, {
      expected: [200],
      tokenQuery: reporter.token,
    });
    assert(detail.messages.length >= 3, 'opening + 2 replies');
    for (let i = 1; i < detail.messages.length; i++) {
      assert(detail.messages[i].id > detail.messages[i - 1].id, 'detail messages ascending by id');
    }
    return `${detail.messages.length} messages, ascending`;
  });

  await step('ticket.detail rejects bogus ticket id', async () => {
    // 不存在的工单 id 必须被 access 守卫拦下（400/403/404 任一），
    // 验证 ensure_ticket_access 不会泄漏或 500。
    await req('GET', `/api/tickets/00000000-0000-0000-0000-000000000000`, {
      expected: [400, 403, 404],
      tokenQuery: reporter.token,
    });
    return 'bogus ticket id rejected';
  });

  writeReport('pass');
  console.log('RESULT pass');
}

main().catch((err) => {
  console.error(`FAIL ${err?.message || err}`);
  writeReport('fail', err);
  process.exit(1);
});
