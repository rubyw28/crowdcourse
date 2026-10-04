#!/usr/bin/env node
// Aid station: one process for the Raspberry Pi (or a laptop that should stay awake).
// Plug one bracelet into USB. This runs the bridge and the dashboard together.
// The walking bracelet never talks to the Pi. Phones still join that bracelet's own Wi-Fi.
//
//   npm run station
'use strict';
const { spawn } = require('child_process');
const os = require('os');
const path = require('path');

const PORT = process.env.PORT || '8787';
let web = null;
let bridge = null;
let stopping = false;

function lanUrls() {
  const out = [];
  for (const list of Object.values(os.networkInterfaces())) {
    for (const n of list || []) {
      if ((n.family === 'IPv4' || n.family === 4) && !n.internal) out.push(n.address);
    }
  }
  return out;
}

function pipe(name, child) {
  const write = (buf) => {
    for (const line of buf.toString().split('\n')) {
      if (line.trim()) console.log(`[${name}] ${line}`);
    }
  };
  child.stdout.on('data', write);
  child.stderr.on('data', write);
}

function startWeb() {
  if (stopping) return;
  web = spawn(process.execPath, [path.join(__dirname, 'web', 'server.js')], {
    cwd: __dirname,
    env: process.env,
  });
  pipe('dashboard', web);
  web.on('exit', (code, signal) => {
    web = null;
    if (stopping) return;
    console.error(`[dashboard] exited (${signal || code}). Restarting in 3s.`);
    setTimeout(startWeb, 3000);
  });
}

function startBridge() {
  if (stopping) return;
  bridge = spawn(process.execPath, [path.join(__dirname, 'bridge', 'bridge.js')], {
    cwd: __dirname,
    env: process.env,
  });
  pipe('bridge', bridge);
  bridge.on('exit', (code) => {
    bridge = null;
    if (stopping) return;
    console.error(`[bridge] exited (${code}). Plug the station bracelet into USB. Retrying in 3s.`);
    setTimeout(startBridge, 3000);
  });
}

function shutdown() {
  stopping = true;
  if (web) web.kill('SIGTERM');
  if (bridge) bridge.kill('SIGTERM');
}

process.on('SIGINT', () => { shutdown(); process.exit(0); });
process.on('SIGTERM', () => { shutdown(); process.exit(0); });

console.log('Crowd Course aid station');
console.log(`  on this machine   http://localhost:${PORT}`);
for (const ip of lanUrls()) console.log(`  on the network    http://${ip}:${PORT}`);
console.log('One bracelet stays on the USB cable. The other one walks.');

startWeb();
startBridge();
