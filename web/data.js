'use strict';
// Every read the station makes from Tiger Data. Each query is timed so the dashboard
// can show what the database is doing, and the same functions back Gemini's tools.
const { scoreOf, labelFor, LABEL } = require('./score');
const { interpret } = require('./model');

const STALE_SEC = 20;

// Runs a query and records how long it took under `name`.
function timer(db) {
  const timings = [];
  return {
    timings,
    async q(name, sql, params) {
      const t = process.hrtime.bigint();
      const res = await db.query(sql, params);
      timings.push({ name, ms: Number(process.hrtime.bigint() - t) / 1e6, rows: res.rowCount });
      return res;
    },
  };
}

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

// Each time a bracelet stopped hearing its friend: how long until it heard them again,
// and what the signal did in the 40 seconds before. A flat, strong signal that stops is
// a band switched off or a body in the way. A falling one is someone walking out of range.
const SEPARATIONS_SQL = `
WITH marks AS (
  SELECT time, bracelet, friend, kind,
         lead(time) OVER w AS next_time,
         lead(kind) OVER w AS next_kind
  FROM events
  WHERE kind IN ('lost', 'found') AND time > now() - make_interval(hours => $1)
    AND ($2::text IS NULL OR (bracelet = $2 AND friend = $3))
  WINDOW w AS (PARTITION BY bracelet, friend ORDER BY time)
)
SELECT m.bracelet, m.friend, m.time AS lost_at,
       CASE WHEN m.next_kind = 'found' THEN m.next_time END AS found_at,
       round(extract(epoch FROM coalesce(CASE WHEN m.next_kind = 'found' THEN m.next_time END, now()) - m.time))::int AS seconds,
       before.last_rssi, before.slope
FROM marks m
LEFT JOIN LATERAL (
  SELECT last(rssi, time) AS last_rssi,
         regr_slope(rssi, extract(epoch FROM time))::real AS slope
  FROM readings r
  WHERE r.bracelet = m.bracelet AND r.friend = m.friend
    AND r.time BETWEEN m.time - interval '40 seconds' AND m.time
) before ON true
WHERE m.kind = 'lost'
ORDER BY m.time DESC
LIMIT 12`;

function howLost(row) {
  if (row.last_rssi == null) return 'no readings before it';
  if (row.slope != null && row.slope <= -0.3) return 'faded out, walking apart';
  if (scoreOf(row.last_rssi) >= 40) return 'stopped while close, band off or blocked';
  return 'dropped at the edge of range';
}

async function separations(db, pair, hours = 12, t = timer(db)) {
  const res = await t.q('separations', SEPARATIONS_SQL, [hours, pair ? pair[0] : null, pair ? pair[1] : null]);
  return res.rows.map((r) => ({
    bracelet: r.bracelet,
    friend: r.friend,
    lostAt: r.lost_at,
    foundAt: r.found_at,
    seconds: r.seconds,
    open: !r.found_at,
    lastScore: r.last_rssi == null ? null : Math.round(scoreOf(r.last_rssi)),
    how: howLost(r),
  }));
}

// Closeness for one pair in 10-second buckets, from the continuous aggregate.
async function history(db, pair, minutes = 10, t = timer(db)) {
  const res = await t.q('history',
    `SELECT bucket AS t, rssi, packets FROM readings_10s
     WHERE bracelet = $1 AND friend = $2 AND bucket > now() - make_interval(mins => $3)
     ORDER BY bucket`,
    [pair[0], pair[1], minutes]);
  return res.rows.map((r) => ({ t: r.t, score: Math.round(scoreOf(Number(r.rssi))), packets: Number(r.packets) }));
}

// How steady the radio link is: median and spread of raw RSSI, and beacons per second.
async function signalStats(db, pair, minutes = 2, t = timer(db)) {
  const res = await t.q('signal_stats',
    `SELECT count(*)::int AS packets,
            percentile_cont(0.5) WITHIN GROUP (ORDER BY rssi)::real AS median,
            stddev_samp(rssi)::real AS spread,
            extract(epoch FROM max(time) - min(time))::real AS span
     FROM readings
     WHERE bracelet = $1 AND friend = $2 AND time > now() - make_interval(mins => $3)`,
    [pair[0], pair[1], minutes]);
  const r = res.rows[0];
  if (!r || !r.packets) return { packets: 0 };
  return {
    packets: r.packets,
    perSecond: r.span > 0 ? Math.round((r.packets / r.span) * 10) / 10 : null,
    medianScore: Math.round(scoreOf(r.median)),
    spreadDb: r.spread == null ? null : Math.round(r.spread * 10) / 10,
  };
}

async function recentEvents(db, minutes = 30, kind = null, t = timer(db)) {
  const res = await t.q('events',
    `SELECT time, bracelet, friend, kind, detail FROM events
     WHERE time > now() - make_interval(mins => $1) AND ($2::text IS NULL OR kind = $2)
     ORDER BY time DESC LIMIT 20`,
    [minutes, kind]);
  return res.rows;
}

async function overview(db) {
  const started = process.hrtime.bigint();
  const t = timer(db);
  const latest = await t.q('latest',
    'SELECT time, bracelet, friend, rssi FROM readings ORDER BY time DESC LIMIT 1');
  // A bracelet can hear up to two friends. The dashboard follows one pair: whoever was heard last.
  const row = latest.rows[0];
  const pair = row ? [row.bracelet, row.friend] : null;

  // Independent reads go out together on the pool.
  const [seriesQ, events, counts, checkin, recent, learned, seps, texts, watching] = await Promise.all([
    pair
      ? t.q('series',
        `SELECT bucket AS t, rssi, packets FROM readings_10s
         WHERE bracelet = $1 AND friend = $2 AND bucket > now() - interval '12 hours'
         ORDER BY bucket`, pair)
      : { rows: [] },
    t.q('events', 'SELECT time, bracelet, friend, kind, detail FROM events ORDER BY time DESC LIMIT 40'),
    t.q('counts',
      `SELECT
         (SELECT count(*)::int FROM readings) AS readings,
         (SELECT count(*)::int FROM readings_10s) AS buckets,
         (SELECT count(*)::int FROM events) AS events`),
    t.q('checkin', 'SELECT time, pulse_bpm, breaths_pm FROM checkins ORDER BY time DESC LIMIT 1'),
    pair
      ? t.q('motion_window',
        `SELECT time, rssi FROM readings
         WHERE time > now() - interval '8 seconds' AND bracelet = $1 AND friend = $2
         ORDER BY time`, pair)
      : { rows: [] },
    t.q('calibration', `SELECT detail FROM events WHERE kind = 'calibrate' ORDER BY time DESC LIMIT 1`),
    separations(db, pair, 12, t),
    t.q('messages', 'SELECT time, handle, direction, body FROM messages ORDER BY time DESC LIMIT 10'),
    t.q('watchers', 'SELECT count(*)::int AS n FROM watchers'),
  ]);

  const series = seriesQ.rows.map((r) => ({
    t: r.t,
    rssi: Math.round(r.rssi),
    score: Math.round(scoreOf(Number(r.rssi))),
    packets: Number(r.packets),
  }));

  let live = null;
  if (row) {
    const ageSec = Math.round((Date.now() - new Date(row.time).getTime()) / 1000);
    const score = Math.round(scoreOf(row.rssi));
    const key = ageSec > STALE_SEC ? 'lost' : labelFor(score);
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
  };
  const motion = interpret(
    recent.rows.map((r) => ({ r: r.rssi, t: new Date(r.time).getTime() })),
    learnedCal,
    Date.now()
  );

  const snapshot = {
    live,
    series,
    reunions: reunions(series),
    separations: seps,
    events: events.rows,
    counts: counts.rows[0],
    checkin: checkin.rows[0] || null,
    messages: texts.rows,
    watchers: watching.rows[0].n,
    motion,
    learned: detail.near != null,
    timings: t.timings,
    dbMs: Number(process.hrtime.bigint() - started) / 1e6,
  };
  snapshot.situation = situationKey(snapshot);
  return snapshot;
}

// Stable id for "something changed." The dashboard writes a new line once per id, not on every poll.
function situationKey(data) {
  const sos = (data.events || []).find((e) => /sos/.test(e.kind));
  if (sos && !/end|acked/.test(sos.kind)) return `sos:${sos.time}`;
  const live = data.live;
  if (!live) return 'empty';
  if (live.state === 'lost') return `lost:${new Date(live.time).toISOString()}`;
  const motion = data.motion && data.motion.motion;
  if (motion === 'blocked' || motion === 'approaching' || motion === 'leaving') {
    return `${motion}:${live.bracelet}:${live.friend}`;
  }
  const band = live.score >= 70 ? 'close' : live.score >= 40 ? 'near' : 'far';
  return `${band}:${live.bracelet}:${live.friend}`;
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
  const apart = (data.separations || []).slice(0, 4).map((s) =>
    `${new Date(s.lostAt).toISOString()} ${s.open ? `still apart after ${s.seconds}s` : `apart ${s.seconds}s`}, ${s.how}`
  ).join('\n');
  const move = data.motion && data.motion.text ? data.motion.text : 'no clear approach or departure in the last few seconds';
  const series = data.series || [];
  let shape = 'No 10-second history in the window.';
  if (series.length) {
    const first = series[0];
    const last = series[series.length - 1];
    const scores = series.map((p) => p.score);
    const drop = first.score - last.score;
    shape = `${series.length} buckets from ${first.score} to ${last.score} (min ${Math.min(...scores)}, max ${Math.max(...scores)}).`;
    if (drop >= 15) shape += ' The score fell sharply across the window.';
    else if (last.score - first.score >= 15) shape += ' The score rose across the window.';
    else shape += ' The score did not travel. This is a moment, not a walk.';
  }
  return `${live}\nHistory: ${shape}\nMotion: ${move}\nBeacons stored: ${data.counts.readings}. 10-second buckets: ${data.counts.buckets}.\nReunions (far back to very close): ${back}.\nSeparations:\n${apart || '(none)'}\nRecent events:\n${recent || '(none)'}`;
}

module.exports = { overview, contextText, separations, history, signalStats, recentEvents };
