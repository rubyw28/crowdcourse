'use strict';
// Presage SmartSpectra on the laptop camera. Video stays on device; we keep the numbers.
// The SDK is optional so the rest of the server runs before `npm install @smartspectra/node-sdk`.

let sdk = null;
let state = { status: 'idle', hint: '', pulse: null, breathing: null, error: null };

function numberNamed(obj, re) {
  let found = null;
  (function walk(o) {
    if (!o || typeof o !== 'object' || found != null) return;
    for (const [k, v] of Object.entries(o)) {
      if (typeof v === 'number' && re.test(k) && v > 0) { found = v; return; }
      walk(v);
    }
  })(obj);
  return found;
}

function sdkInstalled() {
  try { require.resolve('@smartspectra/node-sdk'); return true; } catch (e) { return false; }
}

function snapshot() {
  return {
    status: state.status,
    hint: state.hint,
    pulse: state.pulse,
    breathing: state.breathing,
    error: state.error,
    ready: Boolean(process.env.PRESAGE_API_KEY),
    installed: sdkInstalled(),
  };
}

async function start() {
  const key = process.env.PRESAGE_API_KEY;
  if (!key) throw new Error('PRESAGE_API_KEY is not set. Get one at physiology.presagetech.com');
  let mod;
  try {
    mod = require('@smartspectra/node-sdk');
  } catch (e) {
    throw new Error('Install the Presage SDK first: npm install @smartspectra/node-sdk');
  }
  if (sdk) await stop();
  const { SmartSpectraSDK, CameraSelection, breathingMetrics, cardioMetrics, decodeMetrics } = mod;
  state = { status: 'starting', hint: 'Look at this laptop camera. Sit still.', pulse: null, breathing: null, error: null };
  sdk = new SmartSpectraSDK({
    apiKey: key,
    requestedMetrics: [...breathingMetrics, ...cardioMetrics],
  });
  sdk.on('validationStatus', (_code, _ts, hint) => { state.hint = hint || state.hint; });
  sdk.on('processingStatus', (status) => { state.status = String(status || 'measuring'); });
  sdk.on('metrics', (buf) => {
    let decoded = {};
    try { decoded = decodeMetrics(buf); } catch (e) { decoded = {}; }
    const pulse = numberNamed(decoded, /pulse|heart/i);
    const breathing = numberNamed(decoded, /breath/i);
    if (pulse) state.pulse = Math.round(pulse);
    if (breathing) state.breathing = Math.round(breathing * 10) / 10;
    state.status = 'measuring';
  });
  sdk.on('error', (code, message) => {
    state.error = `${code || ''} ${message || ''}`.trim();
    state.status = 'error';
  });
  sdk.useCamera(CameraSelection.default);
  sdk.start();
  state.status = 'measuring';
  return snapshot();
}

async function stop() {
  const current = sdk;
  sdk = null;
  if (current) {
    try { await current.stopAsync(); } catch (e) { /* already stopped */ }
    try { await current.destroy(); } catch (e) { /* already destroyed */ }
  }
  state.status = 'idle';
  return snapshot();
}

module.exports = { start, stop, snapshot };
