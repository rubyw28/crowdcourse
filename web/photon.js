'use strict';
// Photon Spectrum: the station's iMessage line. Inbound texts arrive on Spectrum's
// message stream, so the station needs no public URL or tunnel.
// Docs: https://photon.codes/spectrum

let ready = null;

function configured() {
  return Boolean(process.env.PHOTON_PROJECT_ID && process.env.PHOTON_SECRET);
}

function connect() {
  if (!configured()) throw new Error('PHOTON_PROJECT_ID and PHOTON_SECRET are not set');
  if (!ready) {
    ready = (async () => {
      const { Spectrum } = await import('spectrum-ts');
      const { imessage } = await import('spectrum-ts/providers/imessage');
      const app = await Spectrum({
        projectId: process.env.PHOTON_PROJECT_ID,
        projectSecret: process.env.PHOTON_SECRET,
        providers: [imessage.config()],
        options: { logLevel: 'warn' },
      });
      return { app, im: imessage(app) };
    })();
    ready.catch(() => { ready = null; });
  }
  return ready;
}

function textOf(message) {
  const c = message && message.content;
  if (!c) return '';
  if (typeof c === 'string') return c;
  return typeof c.text === 'string' ? c.text : '';
}

// Calls onText({ handle, spaceId, text }) for each inbound text and sends back what it returns.
// Reconnects if the stream drops.
async function listen(onText) {
  for (;;) {
    try {
      const { app } = await connect();
      console.log('Photon: listening for iMessages');
      for await (const [space, message] of app.messages) {
        if (message.direction !== 'inbound') continue;
        const text = textOf(message).trim();
        if (!text) continue;
        const handle = (message.sender && message.sender.id) || 'unknown';
        try {
          const reply = await app.responding(space, () => onText({ handle, spaceId: space.id, text }));
          if (reply) await space.send(reply);
        } catch (e) {
          console.error('Photon reply:', e.message);
        }
      }
    } catch (e) {
      console.error('Photon stream:', e.message);
      ready = null;
    }
    await new Promise((r) => setTimeout(r, 5000));
  }
}

// Texts someone first, using their saved conversation when there is one.
async function sendTo(handle, text, spaceId) {
  const { im } = await connect();
  let space = null;
  if (spaceId) space = await im.space.get(spaceId).catch(() => null);
  if (!space) space = await im.space.create(handle);
  await space.send(text);
}

module.exports = { configured, listen, sendTo };
