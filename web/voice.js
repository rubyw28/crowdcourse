'use strict';
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const CACHE = path.join(__dirname, 'cache');

function cachePath(engine, text) {
  const id = crypto.createHash('sha256').update(engine + '\n' + text).digest('hex').slice(0, 24);
  return path.join(CACHE, `${engine}-${id}.mp3`);
}

async function elevenLabs(text) {
  const key = process.env.ELEVENLABS_API_KEY;
  if (!key) throw new Error('ELEVENLABS_API_KEY is not set');
  if (!key.startsWith('sk_')) {
    throw new Error('That ElevenLabs value is the key ID. Create the key again and copy the secret that starts with sk_. It is shown only once.');
  }
  const voice = process.env.ELEVENLABS_VOICE_ID || 'pNInz6obpgDQGcFmaJgB';
  const res = await fetch(`https://api.elevenlabs.io/v1/text-to-speech/${voice}`, {
    method: 'POST',
    headers: {
      'xi-api-key': key,
      'Content-Type': 'application/json',
      Accept: 'audio/mpeg',
    },
    body: JSON.stringify({
      text,
      model_id: 'eleven_flash_v2_5',
      voice_settings: { stability: 0.35, similarity_boost: 0.8, style: 0.45 },
    }),
  });
  if (!res.ok) throw new Error(`ElevenLabs HTTP ${res.status}: ${(await res.text()).slice(0, 180)}`);
  return Buffer.from(await res.arrayBuffer());
}

async function speak(engine, text) {
  const line = String(text || '').trim().slice(0, 400);
  if (!line) throw new Error('Nothing to speak');
  if (engine !== 'elevenlabs') throw new Error('Unknown voice engine');
  fs.mkdirSync(CACHE, { recursive: true });
  const file = cachePath(engine, line);
  if (fs.existsSync(file) && fs.statSync(file).size > 100) return fs.readFileSync(file);
  const audio = await elevenLabs(line);
  fs.writeFileSync(file, audio);
  return audio;
}

function engines() {
  return { elevenlabs: Boolean(process.env.ELEVENLABS_API_KEY) };
}

module.exports = { speak, engines };
