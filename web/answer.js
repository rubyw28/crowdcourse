'use strict';
// Answers for the person helping, written by Gemini from Tiger Data.
// Questions go to an agent that calls tools backed by SQL on Tiger Data and decides what to look up.
// Without GEMINI_API_KEY, or when Gemini fails, the readings answer on their own.
const { labelFor, LABEL } = require('./score');
const { separations, history, signalStats, recentEvents } = require('./data');

const MODELS = ['gemini-3.8-flash', 'gemini-3.5-flash', 'gemini-3.5-flash-lite', 'gemini-flash-latest'];
const MAX_TOOL_ROUNDS = 4;

const SYSTEM = `You are the Crowd Course navigator for a pair of friend-finding bracelets.
The bracelets are for two people a crowd has separated, when a phone cannot get them back. You inform the person trying to help.
Answer in two or three plain sentences. Use only the live data and tool results you are given.
Closeness is 0 to 100: under 40 is far, 40 to 69 is nearby, 70 or more is very close.
If the last reading is more than 20 seconds old, say the signal is stale and give the last known place.
If someone raised an SOS, say so first and tell the asker to go to them.
Do not invent distances in meters. Do not mention being an AI, tools, or databases.`;

// Models that just returned 429 or 503 sit out for a minute instead of costing a failed call each time.
const resting = new Map();

async function callGemini(body) {
  const key = process.env.GEMINI_API_KEY;
  let lastError = 'Gemini returned nothing';
  for (const model of MODELS) {
    if (resting.get(model) > Date.now()) continue;
    const res = await fetch(
      `https://generativelanguage.googleapis.com/v1beta/models/${model}:generateContent`,
      { method: 'POST', headers: { 'Content-Type': 'application/json', 'X-goog-api-key': key }, body: JSON.stringify(body) }
    );
    const json = await res.json().catch(() => ({}));
    if (res.ok) {
      const content = json.candidates && json.candidates[0] && json.candidates[0].content;
      if (content && content.parts) return { content, model };
      continue;
    }
    lastError = json.error && json.error.message ? json.error.message : `Gemini HTTP ${res.status}`;
    if (res.status === 429 || res.status === 503) resting.set(model, Date.now() + 60000);
    if (res.status === 404 || res.status === 429 || res.status === 503 || /high demand|unavailable|overloaded|no longer available/i.test(lastError)) continue;
    throw new Error(lastError);
  }
  throw new Error(lastError);
}

const textOf = (content) => content.parts.map((p) => p.text || '').join('').trim();
const generation = { temperature: 0.2, maxOutputTokens: 1024, thinkingConfig: { thinkingBudget: 0 } };

// What Gemini can look up. Each one is a query on Tiger Data for the pair being followed.
const TOOLS = [
  {
    name: 'closeness_history',
    description: 'Closeness score (0-100) for the pair in 10-second buckets from the continuous aggregate. Use it to tell a walk apart or together from a moment.',
    parameters: { type: 'object', properties: { minutes: { type: 'integer', description: 'How far back, 1 to 720' } }, required: ['minutes'] },
    run: (db, pair, a) => history(db, pair, clamp(a.minutes, 1, 720, 10)),
  },
  {
    name: 'separations',
    description: 'Each time the bracelets stopped hearing each other: when, for how long, whether they are still apart, and how it happened (faded while walking apart, or stopped while close, which means a band switched off or a body in the way).',
    parameters: { type: 'object', properties: { hours: { type: 'integer', description: 'How far back, 1 to 168' } }, required: ['hours'] },
    run: (db, pair, a) => separations(db, pair, clamp(a.hours, 1, 168, 12)),
  },
  {
    name: 'signal_stats',
    description: 'How steady the radio link is over the last minutes: beacons per second, median closeness, and spread in dB. A large spread means people or walls moving between them.',
    parameters: { type: 'object', properties: { minutes: { type: 'integer', description: 'How far back, 1 to 60' } }, required: ['minutes'] },
    run: (db, pair, a) => signalStats(db, pair, clamp(a.minutes, 1, 60, 2)),
  },
  {
    name: 'recent_events',
    description: 'Bracelet events: my_sos, friend_sos, friend_sos_acked, my_sos_acked, lost, found, calibrate.',
    parameters: {
      type: 'object',
      properties: {
        minutes: { type: 'integer', description: 'How far back, 1 to 720' },
        kind: { type: 'string', description: 'Only this kind of event. Leave out for all.' },
      },
      required: ['minutes'],
    },
    run: (db, pair, a) => recentEvents(db, clamp(a.minutes, 1, 720, 30), a.kind || null),
  },
];

function clamp(v, lo, hi, dflt) {
  const n = Math.round(Number(v));
  return Number.isFinite(n) ? Math.min(hi, Math.max(lo, n)) : dflt;
}

// The agent loop: Gemini reads the live snapshot, calls tools until it has enough, then answers.
async function askAgent(question, context, db, pair) {
  const declarations = TOOLS.map(({ name, description, parameters }) => ({ name, description, parameters }));
  const contents = [{ role: 'user', parts: [{ text: `Live data:\n${context}\n\nQuestion: ${question}` }] }];
  const used = [];
  for (let round = 0; round <= MAX_TOOL_ROUNDS; round++) {
    const { content, model } = await callGemini({
      systemInstruction: { parts: [{ text: SYSTEM }] },
      contents,
      tools: pair && round < MAX_TOOL_ROUNDS ? [{ functionDeclarations: declarations }] : undefined,
      generationConfig: generation,
    });
    const calls = content.parts.filter((p) => p.functionCall).map((p) => p.functionCall);
    if (!calls.length) return { text: textOf(content), model, tools: used };
    contents.push(content);
    const results = await Promise.all(calls.map(async (call) => {
      const tool = TOOLS.find((t) => t.name === call.name);
      used.push({ name: call.name, args: call.args || {} });
      const response = tool
        ? await tool.run(db, pair, call.args || {}).then((result) => ({ result }), (e) => ({ error: e.message }))
        : { error: `No tool named ${call.name}` };
      return { functionResponse: { name: call.name, response } };
    }));
    contents.push({ role: 'user', parts: results });
  }
  throw new Error('Gemini kept calling tools without answering');
}

function agoWords(sec) {
  if (sec < 90) return `${sec} seconds`;
  if (sec < 5400) return `${Math.round(sec / 60)} minutes`;
  return `${Math.round(sec / 60 / 6) / 10} hours`;
}

function localAnswer(question, data) {
  const q = question.toLowerCase();
  const live = data.live;
  if (!live) return 'No bracelet readings are in Tiger Data yet.';
  const known = LABEL[labelFor(live.score)].toLowerCase();
  const where = live.state === 'lost'
    ? `${live.bracelet} last heard ${live.friend} ${agoWords(live.ageSec)} ago. The last closeness was ${live.score} out of 100, which is ${known}. The signal is stale now.`
    : `${live.friend} is ${live.label.toLowerCase()} to ${live.bracelet}. Closeness ${live.score} out of 100, RSSI ${live.rssi} dBm.`;
  const sos = data.events.find((e) => /sos/.test(e.kind) && !/end|acked/.test(e.kind));
  if (/sos|help|emergency|hurt/.test(q) || sos) {
    return sos
      ? `${sos.bracelet} raised ${sos.kind.replaceAll('_', ' ')}. ${where}`
      : `No open SOS in the stored events. ${where}`;
  }
  return where;
}

// The JSON schema should hold, but a model that wraps it in prose still gives a usable line.
function briefOf(text) {
  try {
    const parsed = JSON.parse(text.slice(text.indexOf('{'), text.lastIndexOf('}') + 1));
    if (parsed.brief) return String(parsed.brief).trim();
  } catch (e) { /* not JSON */ }
  return text;
}

function localLine(data) {
  const live = data.live;
  if (!live) return 'No friend is in range yet.';
  const sos = (data.events || []).find((e) => /sos/.test(e.kind) && !/end|acked/.test(e.kind));
  if (sos) {
    return `${sos.bracelet || live.friend} raised an SOS while the last closeness was ${live.score}. Go to them and acknowledge it on the band.`;
  }
  if (data.motion && data.motion.motion === 'blocked') {
    return `${live.friend} did not walk away. The signal dropped while beacons kept arriving, so someone stepped between you.`;
  }
  if (live.state === 'lost') {
    return `${live.bracelet} last heard ${live.friend} ${agoWords(live.ageSec)} ago at closeness ${live.score}. That is the last place, not where they are now.`;
  }
  return `${live.friend} is ${live.label.toLowerCase()} to ${live.bracelet}, closeness ${live.score}. Keep walking the way the band gets hotter.`;
}

// The line the dashboard shows when the situation changes, and the body of an SOS text.
// One call, structured output.
async function situationLine(data, context) {
  if (process.env.GEMINI_API_KEY) {
    const line = await callGemini({
      systemInstruction: { parts: [{ text: SYSTEM }] },
      contents: [{ role: 'user', parts: [{ text: `Live data:\n${context}

brief is two sentences for the person helping: what the history shows, then what to do.
A sharp drop while packets keep arriving means a person stepped between them, not that the friend left.
A stale reading is the last place, not the current place.
If an SOS is open, both sentences are about getting to them.
Do not invent a walk the history does not show.` }] }],
      generationConfig: {
        ...generation,
        responseMimeType: 'application/json',
        responseSchema: { type: 'object', properties: { brief: { type: 'string' } }, required: ['brief'] },
      },
    }).catch((e) => { console.error('Gemini line failed, using the readings:', e.message); return null; });
    if (line) return { text: briefOf(textOf(line.content)), source: 'gemini', model: line.model };
  }
  return { text: localLine(data), source: 'readings' };
}

async function answer(question, data, context, db) {
  const q = String(question || '').trim().slice(0, 500);
  if (!q) throw new Error('Ask a question first');
  if (process.env.GEMINI_API_KEY) {
    const pair = data.live ? [data.live.bracelet, data.live.friend] : null;
    const result = await askAgent(q, context, db, pair)
      .catch((e) => { console.error('Gemini answer failed, using the readings:', e.message); return null; });
    if (result && result.text) return { ...result, source: 'gemini' };
  }
  return { text: localAnswer(q, data), source: 'readings', tools: [] };
}

module.exports = { answer, situationLine };
