#!/usr/bin/env node
// Organizer dashboard and the online sponsor features.
// The bracelets keep working with no internet. This process only reads Tiger Data.
//
//   npm run web          # http://localhost:8787
'use strict';
const fs = require('fs');
const http = require('http');
const path = require('path');
const { loadEnv } = require('../lib/env');
const { makePool } = require('../lib/db');
const { overview, contextText } = require('./data');
const { answer } = require('./answer');
const { speak, engines } = require('./voice');
const solana = require('./solana');
const presage = require('./presage');
const photon = require('./photon');

loadEnv();

const PORT = Number(process.env.PORT) || 8787;
const PUBLIC = path.join(__dirname, 'public');
const pages = { '/': 'dashboard.html', '/checkin': 'checkin.html' };

let db = null;
let lastSosAt = new Date();

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

async function watchSos() {
  if (!db || !process.env.EMERGENCY_CONTACTS || !process.env.PHOTON_PROJECT_ID) return;
  try {
    const rows = await db.query(
      `SELECT time, bracelet, friend, kind FROM events
       WHERE kind = 'my_sos' AND time > $1 ORDER BY time`,
      [lastSosAt]
    );
    for (const row of rows.rows) {
      lastSosAt = new Date(row.time);
      const text = `${row.bracelet} raised an SOS${row.friend ? ` while looking for ${row.friend}` : ''}. Open the Crowdsource dashboard.`;
      const sent = await photon.alertContacts(text);
      console.log('SOS texted', sent.join(', ') || '(no contacts)');
    }
  } catch (e) {
    console.error('SOS watch:', e.message);
  }
}

async function handlePhoton(req, res, raw) {
  const headers = Object.fromEntries(Object.entries(req.headers).map(([k, v]) => [k.toLowerCase(), v]));
  const check = photon.verify(raw.toString('utf8'), headers);
  if (!check.ok) return json(res, 401, { error: check.reason });
  const payload = readJson(raw);
  const message = payload.message || {};
  if (photon.remember(message.id)) return json(res, 200, { ok: true, duplicate: true });
  const text = photon.messageText(message);
  const data = await overview(db);
  const result = await answer(text || 'Where is my friend?', data, contextText(data));
  const space = payload.space || {};
  const sender = message.sender || {};
  photon.sendTo(space.id, sender.id, result.text).catch((e) => console.error('Photon reply:', e.message));
  return json(res, 200, { ok: true });
}

const server = http.createServer(async (req, res) => {
  const chunks = [];
  for await (const c of req) chunks.push(c);
  const raw = Buffer.concat(chunks);
  const url = new URL(req.url, `http://127.0.0.1:${PORT}`);
  try {
    if (!db) return json(res, 503, { error: 'Database is not connected' });

    if (req.method === 'GET' && pages[url.pathname]) {
      const file = path.join(PUBLIC, pages[url.pathname]);
      return send(res, 200, fs.readFileSync(file), { 'Content-Type': 'text/html; charset=utf-8' });
    }

    if (req.method === 'GET' && url.pathname === '/api/overview') {
      const data = await overview(db);
      return json(res, 200, { ...data, voice: engines(), solana: solana.status(), presage: presage.snapshot() });
    }

    if (req.method === 'POST' && url.pathname === '/api/ask') {
      const data = await overview(db);
      const result = await answer(readJson(raw).question, data, contextText(data));
      return json(res, 200, result);
    }

    if (req.method === 'POST' && url.pathname === '/api/speak') {
      const body = readJson(raw);
      const audio = await speak(body.engine, body.text);
      return send(res, 200, audio, { 'Content-Type': 'audio/mpeg' });
    }

    if (req.method === 'POST' && url.pathname === '/api/badge') {
      const data = await overview(db);
      const body = readJson(raw);
      const live = data.live || {};
      const minted = await solana.award({
        bracelet: body.bracelet || live.bracelet,
        friend: body.friend || live.friend,
        note: body.note,
      });
      await db.query(
        `INSERT INTO badges (bracelet, friend, signature, mint, explorer, note)
         VALUES ($1, $2, $3, $4, $5, $6)`,
        [live.bracelet || null, live.friend || null, minted.signature, minted.mint, minted.explorer, minted.note]
      );
      return json(res, 200, minted);
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

    if (req.method === 'POST' && url.pathname === '/api/photon') {
      return await handlePhoton(req, res, raw);
    }

    json(res, 404, { error: 'Not found' });
  } catch (err) {
    console.error(url.pathname, err.message);
    json(res, 500, { error: err.message });
  }
});

connect()
  .then(() => {
    server.listen(PORT, () => console.log(`Crowdsource dashboard  http://localhost:${PORT}`));
    setInterval(watchSos, 8000);
  })
  .catch((err) => {
    console.error(err.message);
    process.exit(1);
  });
