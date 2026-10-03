'use strict';
const { labelFor, LABEL } = require('./score');
// Turns a question plus the Tiger Data snapshot into a short answer.
// Gemini writes it when GEMINI_API_KEY is set. Otherwise the numbers speak for themselves.

const MODEL = 'gemini-2.5-flash';

const SYSTEM = `You are the Crowdsource navigator for a pair of friend-finding bracelets.
The bracelets are for two people a crowd has separated, when a phone cannot get them back. The screen you inform is for the person trying to help.
Answer in two or three plain sentences. Use only the live data you are given.
Closeness is 0 to 100: under 40 is far, 40 to 69 is nearby, 70 or more is very close.
If the last reading is more than 20 seconds old, say the signal is stale and give the last known place.
If someone raised an SOS, say so first and tell the asker to go to them.
Do not invent distances in meters. Do not mention being an AI.`;

async function askGemini(question, context) {
  const key = process.env.GEMINI_API_KEY;
  if (!key) return null;
  const res = await fetch(
    `https://generativelanguage.googleapis.com/v1beta/models/${MODEL}:generateContent?key=${encodeURIComponent(key)}`,
    {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        systemInstruction: { parts: [{ text: SYSTEM }] },
        contents: [{ role: 'user', parts: [{ text: `Live data:\n${context}\n\nQuestion: ${question}` }] }],
        generationConfig: { temperature: 0.2, maxOutputTokens: 220 },
      }),
    }
  );
  const body = await res.json().catch(() => ({}));
  if (!res.ok) {
    const msg = body.error && body.error.message ? body.error.message : `Gemini HTTP ${res.status}`;
    throw new Error(msg);
  }
  const parts = body.candidates && body.candidates[0] && body.candidates[0].content && body.candidates[0].content.parts;
  const text = (parts || []).map((p) => p.text || '').join('').trim();
  if (!text) throw new Error('Gemini returned an empty answer');
  return text;
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

async function answer(question, data, context) {
  const q = String(question || '').trim().slice(0, 500);
  if (!q) throw new Error('Ask a question first');
  if (process.env.GEMINI_API_KEY) {
    const text = await askGemini(q, context);
    return { text, source: 'gemini' };
  }
  return { text: localAnswer(q, data), source: 'readings' };
}

module.exports = { answer };
