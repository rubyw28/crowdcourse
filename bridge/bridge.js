#!/usr/bin/env node
// Laptop bridge: reads a Crowdsource bracelet's USB serial telemetry and stores it in Tiger Data.
//
//   npm install
//   npm run bridge                    # auto-detects the bracelet's serial port
//   npm run bridge -- --port /dev/cu.usbserial-0001
//
// Telemetry lines from the firmware look like:  @{"ev":"rssi","me":"Band 94C1","friend":"Band 4661","rssi":-55}
// Without DATABASE_URL in .env it runs as a dry run and just prints what it would store.

'use strict';
const fs = require('fs');
const path = require('path');
const { SerialPort } = require('serialport');
const { ReadlineParser } = require('@serialport/parser-readline');
const { loadEnv } = require('../lib/env');
const { makePool } = require('../lib/db');

loadEnv();
const args = process.argv.slice(2);
const argPort = args.includes('--port') ? args[args.indexOf('--port') + 1] : null;
const VERBOSE = args.includes('--verbose');   // also echo the bracelet's human-readable log
const FLUSH_MS = 500;

const ts = () => new Date().toISOString().slice(11, 23);
const log = (...a) => console.log(`[${ts()}]`, ...a);

// ---------- database ----------
let db = null;
const pending = { readings: [], events: [] };

async function connectDb() {
  if (!process.env.DATABASE_URL) {
    log('No DATABASE_URL in .env: dry run, nothing is stored.');
    return;
  }
  db = makePool();
  await db.query(fs.readFileSync(path.join(__dirname, 'schema.sql'), 'utf8'));
  log('Connected to Tiger Data; schema ready.');
}

async function flush() {
  const r = pending.readings.splice(0);
  const e = pending.events.splice(0);
  if (!db || (!r.length && !e.length)) return;
  try {
    if (r.length) {
      await db.query(
        `INSERT INTO readings (time, bracelet, friend, rssi)
         SELECT * FROM unnest($1::timestamptz[], $2::text[], $3::text[], $4::smallint[])`,
        [r.map((x) => x.time), r.map((x) => x.me), r.map((x) => x.friend), r.map((x) => x.rssi)]
      );
    }
    for (const x of e) {
      await db.query('INSERT INTO events (time, bracelet, friend, kind, detail) VALUES ($1, $2, $3, $4, $5)',
        [x.time, x.me, x.friend, x.ev, x.detail]);
    }
  } catch (err) {
    log('DB write failed (will keep going):', err.message);
  }
}

// ---------- serial ----------
async function findPort() {
  if (argPort) return argPort;
  const ports = await SerialPort.list();
  const hit = ports.find((p) => /usbserial|usbmodem|ttyUSB|ttyACM|SLAB|wchusb/i.test(p.path))
    || ports.find((p) => /^COM\d+/.test(p.path));
  if (!hit) throw new Error('No bracelet found on USB. Plug one in or pass --port.');
  return hit.path;
}

let lastRssiLog = 0, rssiCount = 0;

function onLine(raw) {
  const line = raw.replace(/\r$/, '');
  if (!line.startsWith('@')) {
    if (VERBOSE && line.trim()) log('  bracelet:', line);
    return;
  }
  let m;
  try { m = JSON.parse(line.slice(1)); } catch { return; }
  const time = new Date();
  if (m.ev === 'rssi') {
    pending.readings.push({ time, me: m.me, friend: m.friend, rssi: m.rssi });
    rssiCount++;
    if (Date.now() - lastRssiLog > 5000) {
      log(`${m.me} hears ${m.friend} at ${m.rssi} dBm (${rssiCount} readings in 5 s)`);
      lastRssiLog = Date.now();
      rssiCount = 0;
    }
    return;
  }
  const { ev, me, friend, ...detail } = m;
  pending.events.push({ time, ev, me, friend, detail });
  log(`event ${ev}`, JSON.stringify({ me, friend, ...detail }));
}

async function main() {
  await connectDb();
  setInterval(flush, FLUSH_MS);
  if (args.includes('--stdin')) {
    // Testing without hardware: pipe telemetry lines in, e.g. from a saved serial log.
    require('readline').createInterface({ input: process.stdin }).on('line', onLine).on('close', async () => {
      await flush();
      process.exit(0);
    });
    return;
  }
  const portPath = await findPort();
  // DTR/RTS low so opening the port doesn't reset the ESP32.
  const port = new SerialPort({ path: portPath, baudRate: 115200, hupcl: false });
  port.on('open', () => { port.set({ dtr: false, rts: false }, () => {}); log(`Listening on ${portPath}`); });
  port.on('error', (e) => log('Serial error:', e.message));
  port.on('close', () => { log('Serial port closed; exiting.'); process.exit(1); });
  port.pipe(new ReadlineParser({ delimiter: '\n' })).on('data', onLine);
}

main().catch((e) => { console.error(e.message); process.exit(1); });
