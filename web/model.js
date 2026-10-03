'use strict';
// This pair's radio, learned from their own together/apart calibration.
// A slow slope is someone walking. A sharp drop while packets keep arriving
// is a body in the way, which should not be scored as the friend leaving.

function stdev(xs) {
  if (!xs.length) return 0;
  const mean = xs.reduce((a, b) => a + b, 0) / xs.length;
  return Math.sqrt(xs.reduce((a, b) => a + (b - mean) * (b - mean), 0) / xs.length);
}

function gauss(x, mu, sigma) {
  const s = Math.max(Number(sigma) || 3, 3);
  const z = (x - mu) / s;
  return Math.exp(-0.5 * z * z) / s;
}

function scoreOfModel(rssi, cal) {
  if (!cal || cal.nearSpread == null || cal.farSpread == null) return null;
  const pn = gauss(rssi, cal.near, cal.nearSpread);
  const pf = gauss(rssi, cal.far, cal.farSpread);
  const total = pn + pf;
  if (!total) return null;
  return Math.max(0, Math.min(100, (100 * pn) / total));
}

const TEXT = {
  approaching: 'Getting closer',
  leaving: 'Moving apart',
  blocked: 'Someone is between you. Stay on this line.',
};

function interpret(samples, cal, now) {
  const window = (samples || []).filter((s) => s.t <= now && now - s.t <= 2500);
  if (window.length < 6) return { motion: 'settling', text: '', slope: 0, score: null };
  const n = window.length;
  const k = Math.max(1, Math.floor(n / 4));
  const avg = (arr) => arr.reduce((a, s) => a + s.r, 0) / arr.length;
  const early = avg(window.slice(0, k));
  const late = avg(window.slice(-k));
  const t0 = window[0].t;
  let st = 0, sr = 0, stt = 0, str = 0;
  for (const s of window) {
    const x = (s.t - t0) / 1000;
    st += x;
    sr += s.r;
    stt += x * x;
    str += x * s.r;
  }
  const den = n * stt - st * st;
  const slope = Math.abs(den) < 1e-6 ? 0 : (n * str - st * sr) / den;
  const far = cal && Number.isFinite(cal.far) ? cal.far : -85;
  const newest = window[window.length - 1].r;
  const baselineArr = window.filter((s) => now - s.t > 400 && now - s.t <= 1500).map((s) => s.r).sort((a, b) => a - b);
  const baseline = baselineArr.length ? baselineArr[baselineArr.length >> 1] : newest;
  const cliff = baseline - newest;
  const cliffNeed = Math.max(12, ((cal && cal.nearSpread) || 4) * 2);
  let motion = 'steady';
  if (baselineArr.length >= 3 && cliff >= cliffNeed && newest > far + 3) motion = 'blocked';
  else if (late - early >= 6 && slope >= 4) motion = 'approaching';
  else if (early - late >= 4 && slope <= -3) motion = 'leaving';
  return {
    motion,
    text: TEXT[motion] || '',
    slope: Math.round(slope * 10) / 10,
    score: scoreOfModel(late, cal),
  };
}

module.exports = { stdev, scoreOfModel, interpret };
