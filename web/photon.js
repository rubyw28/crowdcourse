'use strict';
// Photon Spectrum: iMessage in, a navigator answer out.
// Docs: https://photon.codes/docs (Stable). Reply uses spectrum-ts; the webhook only carries JSON.
const crypto = require('crypto');

let appPromise = null;
const seen = new Set();

function verify(raw, headers) {
  const secret = process.env.PHOTON_WEBHOOK_SECRET;
  const ts = headers['x-spectrum-timestamp'];
  const sig = headers['x-spectrum-signature'] || '';
  if (!secret) return { ok: false, reason: 'PHOTON_WEBHOOK_SECRET is not set' };
  if (!ts || !sig) return { ok: false, reason: 'missing Spectrum signature headers' };
  const age = Math.abs(Date.now() / 1000 - Number(ts));
  if (age > 300) return { ok: false, reason: 'stale webhook' };
  const mac = crypto.createHmac('sha256', secret).update(`v0:${ts}:${raw}`).digest('hex');
  const got = sig.replace(/^v0=/, '');
  const a = Buffer.from(mac);
  const b = Buffer.from(got);
  if (a.length !== b.length || !crypto.timingSafeEqual(a, b)) return { ok: false, reason: 'bad signature' };
  return { ok: true };
}

function messageText(message) {
  if (!message) return '';
  const c = message.content;
  if (typeof c === 'string') return c;
  if (c && typeof c.text === 'string') return c.text;
  if (typeof message.text === 'string') return message.text;
  return '';
}

async function spectrum() {
  if (!process.env.PHOTON_PROJECT_ID || !process.env.PHOTON_SECRET) {
    throw new Error('PHOTON_PROJECT_ID and PHOTON_SECRET are not set');
  }
  if (!appPromise) {
    appPromise = (async () => {
      const { Spectrum } = await import('spectrum-ts');
      const { imessage } = await import('spectrum-ts/providers/imessage');
      return {
        app: await Spectrum({
          projectId: process.env.PHOTON_PROJECT_ID,
          projectSecret: process.env.PHOTON_SECRET,
          providers: [imessage.config()],
          webhookSecret: process.env.PHOTON_WEBHOOK_SECRET,
        }),
        imessage,
      };
    })();
  }
  return appPromise;
}

async function sendTo(spaceId, phone, text) {
  const { app, imessage } = await spectrum();
  const im = imessage(app);
  let space = null;
  if (spaceId) {
    try { space = await im.space.get(spaceId); } catch (e) { space = null; }
  }
  if (!space && phone) {
    const user = await im.user(phone);
    space = await im.space.create(user);
  }
  if (!space) throw new Error('Could not open an iMessage conversation');
  await space.send(text);
}

async function alertContacts(text) {
  const numbers = (process.env.EMERGENCY_CONTACTS || '').split(',').map((s) => s.trim()).filter(Boolean);
  const sent = [];
  for (const phone of numbers) {
    await sendTo(null, phone, text);
    sent.push(phone);
  }
  return sent;
}

function remember(id) {
  if (!id) return false;
  if (seen.has(id)) return true;
  seen.add(id);
  if (seen.size > 500) seen.delete(seen.values().next().value);
  return false;
}

module.exports = { verify, messageText, sendTo, alertContacts, remember };
