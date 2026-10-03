'use strict';
const { scoreOf, labelFor, LABEL } = require('./score');
const { interpret } = require('./model');

const STALE_SEC = 20;

function reunions(series) {
  const out = [];
  let apartAt = null;
  for (const p of series) {
    if (p.score < 40) {
      if (!apartAt) apartAt = p.t;
    } else if (p.score >= 70 && apartAt) {
      out.push({
        from: apartAt,
        to: p.t,
        seconds: Math.round((new Date(p.t) - new Date(apartAt)) / 1000),
      });
      apartAt = null;
    }
  }
  return out;
}

async function overview(db) {
  const latest = await db.query(
    'SELECT time, bracelet, friend, rssi FROM readings ORDER BY time DESC LIMIT 1'
  );
  let seriesQ = await db.query(
    `SELECT bucket AS t, bracelet, friend, rssi, packets
     FROM readings_10s
     WHERE bucket > now() - interval '12 hours'
     ORDER BY bucket`
  );
  if (!seriesQ.rows.length) {
    seriesQ = await db.query(
      `SELECT time_bucket('10 seconds', time) AS t, bracelet, friend,
              avg(rssi)::real AS rssi, count(*)::int AS packets
       FROM readings
       WHERE time > now() - interval '12 hours'
       GROUP BY 1, 2, 3
       ORDER BY 1`
    );
  }
  const events = await db.query(
    `SELECT time, bracelet, friend, kind, detail
     FROM events ORDER BY time DESC LIMIT 40`
  );
  const counts = await db.query(
    `SELECT
       (SELECT count(*)::int FROM readings) AS readings,
       (SELECT count(*)::int FROM readings_10s) AS buckets,
       (SELECT count(*)::int FROM events) AS events`
  );
  const badges = await db.query(
    'SELECT time, bracelet, friend, signature, mint, explorer, note FROM badges ORDER BY time DESC LIMIT 12'
  );
  const checkin = await db.query(
    'SELECT time, pulse_bpm, breaths_pm FROM checkins ORDER BY time DESC LIMIT 1'
  );
  const recent = await db.query(
    `SELECT time, rssi FROM readings
     WHERE time > now() - interval '8 seconds'
     ORDER BY time`
  );
  const learned = await db.query(
    `SELECT detail FROM events WHERE kind = 'calibrate' ORDER BY time DESC LIMIT 1`
  );

  const series = seriesQ.rows.map((r) => ({
    t: r.t,
    bracelet: r.bracelet,
    friend: r.friend,
    rssi: Math.round(r.rssi),
    score: Math.round(scoreOf(Number(r.rssi))),
    packets: Number(r.packets),
  }));

  let live = null;
  if (latest.rows[0]) {
    const row = latest.rows[0];
    const ageSec = Math.round((Date.now() - new Date(row.time).getTime()) / 1000);
    const score = Math.round(scoreOf(row.rssi));
    const stale = ageSec > STALE_SEC;
    const key = stale ? 'lost' : labelFor(score);
    live = {
      time: row.time,
      bracelet: row.bracelet,
      friend: row.friend,
      rssi: row.rssi,
      score,
      state: key,
      label: LABEL[key],
      ageSec,
    };
  }

  const detail = (learned.rows[0] && learned.rows[0].detail) || {};
  const learnedCal = {
    near: Number.isFinite(Number(detail.near)) ? Number(detail.near) : -45,
    far: Number.isFinite(Number(detail.far)) ? Number(detail.far) : -85,
    nearSpread: detail.nearSpread == null ? null : Number(detail.nearSpread),
    farSpread: detail.farSpread == null ? null : Number(detail.farSpread),
  };
  const motion = interpret(
    recent.rows.map((r) => ({ r: r.rssi, t: new Date(r.time).getTime() })),
    learnedCal,
    Date.now()
  );

  return {
    live,
    series,
    reunions: reunions(series),
    events: events.rows,
    counts: counts.rows[0],
    badges: badges.rows,
    checkin: checkin.rows[0] || null,
    motion,
    learned: detail.near != null,
  };
}

function contextText(data) {
  const live = data.live
    ? `${data.live.bracelet} last heard ${data.live.friend} at ${data.live.rssi} dBm, score ${data.live.score}/100 (${data.live.label}), ${data.live.ageSec}s ago.`
    : 'No readings yet.';
  const recent = data.events.slice(0, 8).map((e) =>
    `${new Date(e.time).toISOString()} ${e.kind} ${e.bracelet || ''} ${e.friend || ''}`.trim()
  ).join('\n');
  const back = data.reunions.length
    ? data.reunions.map((r) => `${r.seconds}s`).join(', ')
    : 'none in this window';
  const move = data.motion && data.motion.text ? data.motion.text : 'no clear approach or departure in the last few seconds';
  return `${live}\nMotion: ${move}\nBeacons stored: ${data.counts.readings}. 10-second buckets: ${data.counts.buckets}.\nReunions (far back to very close): ${back}.\nRecent events:\n${recent || '(none)'}`;
}

module.exports = { overview, contextText, reunions };
