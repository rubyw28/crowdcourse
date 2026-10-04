#!/usr/bin/env node
// Mock bracelet: serves index.html and speaks the same WebSocket protocol as the ESP32.
// Zero dependencies (raw RFC 6455 framing) so it runs with plain `node mock-server.js`.
//
//   PORT=8080 FRIEND=Alex SOS_EVERY=60 node mock-server.js
//
// Keys while running: s = friend sends SOS, c = friend cancels SOS,
//                     n = friend walks close, f = friend walks far,
//                     l = toggle signal lost, q = quit

'use strict';
const http = require('http');
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const os = require('os');

const PORT = Number(process.env.PORT) || 8080;
const FRIEND = process.env.FRIEND || 'Alex';
const SOS_EVERY_S = Number(process.env.SOS_EVERY) || 60; // average seconds between simulated SOS
const RSSI_HZ = 5;
const STATUS_MS = 2000;

// Log-distance path loss: rssi = TX_1M - 10 * N * log10(d)
const TX_1M = -45;
const PATH_N = 2.6;
const RSSI_MIN = -90;
const RSSI_MAX = -40;

const INDEX = path.join(__dirname, 'index.html');
const DEMO = path.join(__dirname, 'demo', 'stage.html');
const WS_GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

// ---------- helpers ----------
const rand = (a, b) => a + Math.random() * (b - a);
const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
function gauss() {
  let u = 0, v = 0;
  while (u === 0) u = Math.random();
  while (v === 0) v = Math.random();
  return Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * v);
}
const ts = () => new Date().toISOString().slice(11, 23);
const log = (...a) => console.log(`[${ts()}]`, ...a);

// ---------- HTTP ----------
const server = http.createServer((req, res) => {
  const url = req.url.split('?')[0];
  const file = url === '/' || url === '/index.html' ? INDEX : url === '/demo' || url === '/demo/' ? DEMO : null;
  if (file) {
    fs.readFile(file, (err, buf) => {
      if (err) { res.writeHead(500); res.end(path.basename(file) + ' not found'); return; }
      res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' });
      res.end(buf);
    });
    return;
  }
  res.writeHead(404, { 'Content-Type': 'text/plain' });
  res.end('not found');
});

// ---------- minimal WebSocket server ----------
const clients = new Set();

function encodeFrame(payload, opcode = 0x1) {
  const data = Buffer.isBuffer(payload) ? payload : Buffer.from(payload);
  let header;
  if (data.length < 126) {
    header = Buffer.from([0x80 | opcode, data.length]);
  } else if (data.length < 65536) {
    header = Buffer.alloc(4);
    header[0] = 0x80 | opcode; header[1] = 126; header.writeUInt16BE(data.length, 2);
  } else {
    header = Buffer.alloc(10);
    header[0] = 0x80 | opcode; header[1] = 127; header.writeBigUInt64BE(BigInt(data.length), 2);
  }
  return Buffer.concat([header, data]);
}

function send(sock, obj) {
  if (!sock.destroyed) sock.write(encodeFrame(JSON.stringify(obj)));
}
function broadcast(obj) {
  for (const s of clients) send(s, obj);
}

server.on('upgrade', (req, sock) => {
  const url = req.url.split('?')[0];
  const key = req.headers['sec-websocket-key'];
  if (url !== '/ws' || !key) { sock.end('HTTP/1.1 400 Bad Request\r\n\r\n'); return; }

  const accept = crypto.createHash('sha1').update(key + WS_GUID).digest('base64');
  sock.write(
    'HTTP/1.1 101 Switching Protocols\r\n' +
    'Upgrade: websocket\r\nConnection: Upgrade\r\n' +
    `Sec-WebSocket-Accept: ${accept}\r\n\r\n`
  );
  sock.setNoDelay(true);
  clients.add(sock);
  const who = `${req.socket.remoteAddress}:${req.socket.remotePort}`;
  log(`+ client ${who} (${clients.size} connected)`);
  sendStatus(sock);

  let buf = Buffer.alloc(0);
  sock.on('data', (chunk) => {
    buf = Buffer.concat([buf, chunk]);
    // Parse as many complete frames as are buffered (no fragmentation support; browsers don't fragment small text).
    for (;;) {
      if (buf.length < 2) return;
      const opcode = buf[0] & 0x0f;
      const masked = (buf[1] & 0x80) !== 0;
      let len = buf[1] & 0x7f;
      let off = 2;
      if (len === 126) { if (buf.length < 4) return; len = buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (buf.length < 10) return; len = Number(buf.readBigUInt64BE(2)); off = 10; }
      const maskLen = masked ? 4 : 0;
      if (buf.length < off + maskLen + len) return;
      const mask = masked ? buf.slice(off, off + 4) : null;
      const payload = Buffer.from(buf.slice(off + maskLen, off + maskLen + len));
      if (mask) for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i & 3];
      buf = buf.slice(off + maskLen + len);

      if (opcode === 0x1) onClientMessage(sock, who, payload.toString('utf8'));
      else if (opcode === 0x8) { sock.end(encodeFrame(Buffer.alloc(0), 0x8)); return; }
      else if (opcode === 0x9) sock.write(encodeFrame(payload, 0xA));
    }
  });
  const drop = () => {
    if (clients.delete(sock)) log(`- client ${who} (${clients.size} connected)`);
  };
  sock.on('close', drop);
  sock.on('error', drop);
});

// ---------- simulated world ----------
const world = {
  d: 4,              // metres to friend
  dest: 4,
  speed: 1,
  nextDest: 0,
  shadow: 0,         // slow body-blocking fade (dB), Ornstein-Uhlenbeck
  rssi: null,
  forceLost: false,
  lostUntil: 0,
  nextLost: Date.now() + rand(60e3, 120e3),
  battery: 92,
  friendBattery: 67,
  friendSos: false,
  nextSos: Date.now() + rand(0.6, 1.4) * SOS_EVERY_S * 1000,
};

function step() {
  const now = Date.now();
  const dt = 1 / RSSI_HZ;

  // Friend wanders: sometimes comes close, sometimes drifts across the venue.
  if (now > world.nextDest) {
    world.dest = Math.random() < 0.4 ? rand(0.3, 2.5) : rand(3, 45);
    world.speed = rand(0.4, 1.4);
    world.nextDest = now + rand(8e3, 20e3);
  }
  const delta = world.dest - world.d;
  world.d += Math.sign(delta) * Math.min(Math.abs(delta), world.speed * dt);

  world.shadow += -world.shadow * 0.05 + gauss() * 0.6;

  const ideal = TX_1M - 10 * PATH_N * Math.log10(Math.max(world.d, 0.3));
  world.rssi = Math.round(clamp(ideal + world.shadow + gauss() * 2.5, RSSI_MIN, RSSI_MAX));

  // Occasional full dropout (friend went behind a wall / out of range).
  if (now > world.nextLost) {
    world.lostUntil = now + rand(6e3, 10e3);
    world.nextLost = now + rand(60e3, 120e3);
    log(`~ simulating signal loss for ${Math.round((world.lostUntil - now) / 1000)}s`);
  }
  const lost = world.forceLost || now < world.lostUntil;
  // Weak signals lose packets, like real ESP-NOW near the edge of range.
  const lossP = clamp((-world.rssi - 82) / 10, 0, 0.85);
  if (!lost && Math.random() >= lossP) broadcast({ type: 'rssi', rssi: world.rssi });

  if (now > world.nextSos && !world.friendSos) {
    friendSos(true);
    world.nextSos = now + rand(0.6, 1.4) * SOS_EVERY_S * 1000;
  }
}

function sendStatus(sock) {
  const msg = {
    type: 'status',
    name: FRIEND,
    battery: Math.round(world.battery),
    friendBattery: Math.round(world.friendBattery),
  };
  if (sock) send(sock, msg); else broadcast(msg);
}

function friendSos(on) {
  world.friendSos = on;
  broadcast({ type: on ? 'sos' : 'sos_clear' });
  log(on ? `! ${FRIEND} sent SOS` : `! ${FRIEND} cleared SOS`);
}

function onClientMessage(sock, who, text) {
  log(`< ${who} ${text}`);
  let m;
  try { m = JSON.parse(text); } catch { return; }
  switch (m.type) {
    case 'sos':
      // Pretend the friend sees it on their bracelet and acknowledges a moment later.
      setTimeout(() => broadcast({ type: 'sos_ack' }), rand(2000, 4000));
      break;
    case 'sos_ack':
      world.friendSos = false;
      break;
  }
}

setInterval(step, 1000 / RSSI_HZ);
setInterval(() => {
  world.battery = Math.max(0, world.battery - 0.02);
  world.friendBattery = Math.max(0, world.friendBattery - 0.03);
  sendStatus();
}, STATUS_MS);
setInterval(() => {
  if (clients.size) {
    log(`  d=${world.d.toFixed(1)}m rssi=${world.rssi} lost=${world.forceLost || Date.now() < world.lostUntil} clients=${clients.size}`);
  }
}, 5000);

// ---------- keyboard controls ----------
if (process.stdin.isTTY) {
  process.stdin.setRawMode(true);
  process.stdin.setEncoding('utf8');
  process.stdin.on('data', (k) => {
    switch (k) {
      case 's': friendSos(true); break;
      case 'c': friendSos(false); break;
      case 'n': world.dest = 0.5; world.speed = 3; world.nextDest = Date.now() + 30e3; log('~ friend walking close'); break;
      case 'f': world.dest = 40; world.speed = 4; world.nextDest = Date.now() + 30e3; log('~ friend walking far'); break;
      case 'l': world.forceLost = !world.forceLost; log(`~ signal lost: ${world.forceLost}`); break;
      case 'q': case '\u0003': process.exit(0);
    }
  });
}

server.listen(PORT, () => {
  console.log(`Mock Crowdsource bracelet running. Open on this machine: http://localhost:${PORT}`);
  for (const ifs of Object.values(os.networkInterfaces())) {
    for (const a of ifs || []) {
      if (a.family === 'IPv4' && !a.internal) console.log(`  on your phone (same Wi-Fi): http://${a.address}:${PORT}`);
    }
  }
  console.log(`Scripted demo for presenting: http://localhost:${PORT}/demo`);
  console.log('Keys: s=friend SOS  c=clear SOS  n=walk close  f=walk far  l=toggle lost  q=quit\n');
});
