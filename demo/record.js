#!/usr/bin/env node
// Records demo/stage.html to an MP4 using headless Chrome.
//
//   npm i --no-save puppeteer-core ffmpeg-static
//   node demo/record.js [out.mp4]
//
// Env: CHROME=/path/to/chrome (defaults to the macOS Google Chrome install)

'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawn, spawnSync } = require('child_process');
const puppeteer = require('puppeteer-core');
const ffmpeg = require('ffmpeg-static');

const ROOT = path.join(__dirname, '..');
const OUT = path.resolve(process.argv[2] || path.join(__dirname, 'demo.mp4'));
const CHROME = process.env.CHROME || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const PORT = 8123;
const FPS = 30;

(async () => {
  const mock = spawn(process.execPath, [path.join(ROOT, 'mock-server.js')], { env: { ...process.env, PORT: String(PORT) }, stdio: 'ignore' });
  await new Promise((r) => setTimeout(r, 800));

  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'demo-frames-'));
  const browser = await puppeteer.launch({ executablePath: CHROME, headless: 'new', args: ['--hide-scrollbars', '--force-color-profile=srgb'] });
  const page = await browser.newPage();
  await page.setViewport({ width: 1920, height: 1080, deviceScaleFactor: 1 });

  const cdp = await page.createCDPSession();
  const frames = [];
  cdp.on('Page.screencastFrame', async ({ data, metadata, sessionId }) => {
    const file = path.join(dir, String(frames.length).padStart(6, '0') + '.jpg');
    fs.writeFileSync(file, Buffer.from(data, 'base64'));
    frames.push({ file, t: metadata.timestamp });
    cdp.send('Page.screencastFrameAck', { sessionId }).catch(() => {});
  });

  await page.goto(`http://localhost:${PORT}/demo`);
  await cdp.send('Page.startScreencast', { format: 'jpeg', quality: 95, maxWidth: 1920, maxHeight: 1080, everyNthFrame: 1 });
  process.stdout.write('recording');
  const tick = setInterval(() => process.stdout.write('.'), 2000);
  await page.waitForFunction('window.__done === true', { timeout: 180000, polling: 250 });
  clearInterval(tick);
  await cdp.send('Page.stopScreencast');
  await browser.close();
  mock.kill();
  console.log(`\n${frames.length} frames captured`);

  // Frames arrive only when something changes, so give each one its real on-screen duration.
  const list = frames.map((f, i) => {
    const next = frames[i + 1];
    const dur = next ? Math.max(0.001, next.t - f.t) : 0.5;
    return `file '${f.file}'\nduration ${dur.toFixed(4)}`;
  }).join('\n') + `\nfile '${frames[frames.length - 1].file}'\n`;
  fs.writeFileSync(path.join(dir, 'list.txt'), list);

  const r = spawnSync(ffmpeg, [
    '-y', '-loglevel', 'error', '-f', 'concat', '-safe', '0', '-i', path.join(dir, 'list.txt'),
    '-vf', `fps=${FPS},format=yuv420p`, '-c:v', 'libx264', '-preset', 'slow', '-crf', '18',
    '-movflags', '+faststart', OUT,
  ], { stdio: 'inherit' });
  fs.rmSync(dir, { recursive: true, force: true });
  if (r.status !== 0) process.exit(r.status || 1);
  console.log('wrote ' + OUT);
})().catch((e) => { console.error(e); process.exit(1); });
