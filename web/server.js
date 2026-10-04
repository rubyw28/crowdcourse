#!/usr/bin/env node
// Aid station dashboard, the iMessage line, and the responder check-in.
// The bracelets keep working with no internet. This process reads Tiger Data;
// the bridge is what writes the bracelet's readings into it.
//
//   npm run web          # http://localhost:8787
'use strict';
const fs = require('fs');
const http = require('http');
const path = require('path');
const { loadEnv } = require('../lib/env');
const { makePool } = require('../lib/db');
const { overview, contextText, separations } = require('./data');
const { answer, situationLine } = require('./answer');
const presage = require('./presage');
const photon = require('./photon');

loadEnv();

const PORT = Number(process.env.PORT) || 8787;
const PUBLIC = path.join(__dirname, 'public');
const pages = { '/': 'dashboard.html', '/checkin': 'checkin.html' };

let db = null;
let lineCache = { key: '', at: 0, line: null };

async function currentLine(data) {
  const key = data.situation;
  if (lineCache.key === key && lineCache.line && Date.now() - lineCache.at < 60000) {
    return { ...lineCache.line, key };
  }
  const line = await situationLine(data, contextText(data));
  lineCache = { key, at: Date.now(), line };
  return { ...line, key };
}

function send(res, status, body, headers) {
  const buf = Buffer.isBuffer(body) ? body : Buffer.from(body);
  res.writeHead(status, { 'Content-Length': buf.length, 'Cache-Control': 'no-store', ...headers });
  res.end(buf);
}
function json(res, status, obj) {
  send(res, status, JSON.stringify(obj), { 'Content-Type': 'application/json' });
}
function readJson(raw) {
  if (!raw.length) return {};
  try { return JSON.parse(raw.toString('utf8')); } catch (e) { return {}; }
}

async function connect() {
  if (!process.env.DATABASE_URL) throw new Error('DATABASE_URL is not set');
  db = makePool();
  await db.query(fs.readFileSync(path.join(__dirname, '..', 'bridge', 'schema.sql'), 'utf8'));
}

// ---------- iMessage (Photon) ----------

async function logText(handle, direction, body, detail = {}) {
  await db.query('INSERT INTO messages (handle, direction, body, detail) VALUES ($1, $2, $3, $4)',
    [handle, direction, body, JSON.stringify(detail)]).catch((e) => console.error('Message log:', e.message));
}

// One inbound text: WATCH and STOP manage alerts, anything else is a question for the agent.
async function onText({ handle, spaceId, text }) {
  await logText(handle, 'in', text);
  let reply;
  let detail = {};
  if (/^\s*watch\b/i.test(text)) {
    await db.query(
      `INSERT INTO watchers (handle, space_id) VALUES ($1, $2)
       ON CONFLICT (handle) DO UPDATE SET space_id = EXCLUDED.space_id`,
      [handle, spaceId || null]);
    const data = await overview(db);
    const who = data.live ? `${data.live.bracelet} and ${data.live.friend}` : 'the bracelets';
    reply = `You're watching ${who}. I'll text you if one raises SOS, drops out of range, or comes back. Text STOP to end.`;
  } else if (/^\s*(stop|unwatch)\b/i.test(text)) {
    await db.query('DELETE FROM watchers WHERE handle = $1', [handle]);
    reply = 'Stopped. Text WATCH to get alerts again.';
  } else {
    const data = await overview(db);
    const result = await answer(text, data, contextText(data), db);
    reply = result.text;
    detail = { source: result.source, model: result.model, tools: (result.tools || []).map((t) => t.name) };
  }
  await logText(handle, 'out', reply, detail);
  return reply;
}

async function alertWatchers(text, kind) {
  const contacts = (process.env.EMERGENCY_CONTACTS || '').split(',').map((s) => s.trim()).filter(Boolean);
  const watchers = (await db.query('SELECT handle, space_id FROM watchers')).rows;
  const to = new Map(contacts.map((h) => [h, null]));
  for (const w of watchers) to.set(w.handle, w.space_id);
  if (!to.size || !photon.configured()) {
    console.log('alert', kind, text);
    return;
  }
  for (const [handle, spaceId] of to) {
    try {
      await photon.sendTo(handle, text, spaceId);
      await logText(handle, 'out', text, { alert: kind });
    } catch (e) {
      console.error(`Alert to ${handle}:`, e.message);
    }
  }
}

function minutesWords(sec) {
  if (sec < 90) return `${sec} seconds`;
  return `${Math.floor(sec / 60)} min ${sec % 60} s`;
}

// Turns a new bracelet event into the text a watcher gets, or null for events nobody is told about.
async function alertFor(row) {
  if (row.kind === 'my_sos' || row.kind === 'friend_sos') {
    const line = await currentLine(await overview(db));
    return `SOS from ${row.kind === 'my_sos' ? row.bracelet : row.friend}. ${line.text}`;
  }
  if (row.kind === 'lost' || row.kind === 'found') {
    const [sep] = await separations(db, [row.bracelet, row.friend], 12);
    if (!sep) return null;
    if (row.kind === 'lost') {
      const close = sep.lastScore == null ? '' : ` Last closeness ${sep.lastScore} of 100.`;
      const how = sep.lastScore == null ? '' : ` It ${sep.how}.`;
      return `${row.bracelet} stopped hearing ${row.friend}.${close}${how} Start from where they last stood.`;
    }
    if (!sep.foundAt) return null;
    return `${row.bracelet} hears ${row.friend} again after ${minutesWords(sep.seconds)} apart.`;
  }
  return null;
}

let seenEventAt = new Date();
const lastAlert = new Map();

async function watchEvents() {
  try {
    const rows = await db.query(
      `SELECT time, bracelet, friend, kind FROM events
       WHERE time > $1 AND kind IN ('my_sos', 'friend_sos', 'lost', 'found')
       ORDER BY time`,
      [seenEventAt]
    );
    for (const row of rows.rows) {
      seenEventAt = new Date(row.time);
      // A link at the edge of range flaps between lost and found. One text per pair and kind per 30 s.
      const key = `${row.kind}:${row.bracelet}:${row.friend}`;
      if (!/sos/.test(row.kind) && Date.now() - (lastAlert.get(key) || 0) < 30000) continue;
      const text = await alertFor(row);
      if (!text) continue;
      lastAlert.set(key, Date.now());
      await alertWatchers(text, row.kind);
    }
  } catch (e) {
    console.error('Event watch:', e.message);
  }
  // Scheduled after each pass, never on a fixed interval: an SOS alert waits on Gemini,
  // and an overlapping pass would text the same event twice.
  setTimeout(watchEvents, 3000);
}

const server = http.createServer(async (req, res) => {
  const chunks = [];
  for await (const c of req) chunks.push(c);
  const raw = Buffer.concat(chunks);
  const url = new URL(req.url, `http://127.0.0.1:${PORT}`);
  try {
    if (req.method === 'GET' && pages[url.pathname]) {
      const file = path.join(PUBLIC, pages[url.pathname]);
      return send(res, 200, fs.readFileSync(file), { 'Content-Type': 'text/html; charset=utf-8' });
    }

    if (req.method === 'GET' && url.pathname === '/api/overview') {
      const data = await overview(db);
      return json(res, 200, { ...data, presage: presage.snapshot(), photon: photon.configured() });
    }

    if (req.method === 'GET' && url.pathname === '/api/line') {
      const data = await overview(db);
      return json(res, 200, await currentLine(data));
    }

    if (req.method === 'POST' && url.pathname === '/api/ask') {
      const data = await overview(db);
      const result = await answer(readJson(raw).question, data, contextText(data), db);
      return json(res, 200, result);
    }

    if (req.method === 'POST' && url.pathname === '/api/checkin/start') {
      return json(res, 200, await presage.start());
    }
    if (req.method === 'POST' && url.pathname === '/api/checkin/stop') {
      const snap = presage.snapshot();
      if (snap.pulse || snap.breathing) {
        await db.query(
          'INSERT INTO checkins (pulse_bpm, breaths_pm, detail) VALUES ($1, $2, $3)',
          [snap.pulse, snap.breathing, JSON.stringify({ hint: snap.hint })]
        );
      }
      return json(res, 200, await presage.stop());
    }
    if (req.method === 'GET' && url.pathname === '/api/checkin') {
      return json(res, 200, presage.snapshot());
    }

    json(res, 404, { error: 'Not found' });
  } catch (err) {
    console.error(url.pathname, err.message);
    json(res, 500, { error: err.message });
  }
});

connect()
  .then(() => {
    server.listen(PORT, () => console.log(`Crowd Course dashboard  http://localhost:${PORT}`));
    watchEvents();
    if (photon.configured()) photon.listen(onText);
  })
  .catch((err) => {
    console.error(err.message);
    process.exit(1);
  });
