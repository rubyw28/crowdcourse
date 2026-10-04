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

// Runs a child script and starts it again 3 s after it exits, until the station stops.
const children = new Map();
function keepRunning(name, script, hint) {
  if (stopping) return;
  const child = spawn(process.execPath, [path.join(__dirname, script)], { cwd: __dirname, env: process.env });
  children.set(name, child);
  pipe(name, child);
  child.on('exit', (code, signal) => {
    children.delete(name);
    if (stopping) return;
    console.error(`[${name}] exited (${signal || code}).${hint ? ` ${hint}` : ''} Restarting in 3s.`);
    setTimeout(() => keepRunning(name, script, hint), 3000);
  });
}

function shutdown() {
  stopping = true;
  for (const child of children.values()) child.kill('SIGTERM');
}

process.on('SIGINT', () => { shutdown(); process.exit(0); });
process.on('SIGTERM', () => { shutdown(); process.exit(0); });

console.log('Crowd Course aid station');
console.log(`  on this machine   http://localhost:${PORT}`);
for (const ip of lanUrls()) console.log(`  on the network    http://${ip}:${PORT}`);
console.log('One bracelet stays on the USB cable. The other one walks.');

keepRunning('dashboard', 'web/server.js');
keepRunning('bridge', 'bridge/bridge.js', 'Plug the station bracelet into USB.');
