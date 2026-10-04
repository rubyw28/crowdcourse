'use strict';
// Same RSSI map as the bracelet page (index.html). Near and far are the default calibration.
const NEAR = -45;
const FAR = -85;

function scoreOf(rssi) {
  const s = (100 * (rssi - FAR)) / (NEAR - FAR);
  return Math.max(0, Math.min(100, s));
}

function labelFor(score) {
  if (score >= 70) return 'close';
  if (score >= 40) return 'near';
  return 'far';
}

const LABEL = { close: 'Very close', near: 'Nearby', far: 'Far', lost: 'Lost' };

module.exports = { scoreOf, labelFor, LABEL };
