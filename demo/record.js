#!/usr/bin/env node
// Records demo/stage.html to an MP4 using headless Chrome, then adds the soundtrack
// (synthesized music + on-screen sound effects, see make_audio.py).
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
  process.on('exit', () => mock.kill());
  await new Promise((r) => setTimeout(r, 800));

  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'demo-frames-'));
  const browser = await puppeteer.launch({ executablePath: CHROME, headless: 'new', args: ['--hide-scrollbars', '--force-color-profile=srgb'] });
  const page = await browser.newPage();
  await page.setViewport({ width: 1920, height: 1080, deviceScaleFactor: 1 });
  page.on('pageerror', (e) => console.error('\npage error: ' + e.message));

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
  const events = await page.evaluate(() => window.__events || []);
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
    '-movflags', '+faststart', path.join(dir, 'silent.mp4'),
  ], { stdio: 'inherit' });
  if (r.status !== 0) process.exit(r.status || 1);

  // Soundtrack: cue times relative to the first frame.
  const t0 = frames[0].t;
  const duration = frames[frames.length - 1].t - t0 + 0.5;
  fs.writeFileSync(path.join(dir, 'timeline.json'), JSON.stringify({
    duration,
    events: events.map((e) => ({ ...e, t: e.t - t0 })).filter((e) => e.t >= 0),
  }));
  const a = spawnSync('python3', [path.join(__dirname, 'make_audio.py'), path.join(dir, 'timeline.json'),
    path.join(dir, 'audio.wav')], { stdio: 'inherit' });
  if (a.status !== 0) process.exit(a.status || 1);
  const m = spawnSync(ffmpeg, [
    '-y', '-loglevel', 'error', '-i', path.join(dir, 'silent.mp4'), '-i', path.join(dir, 'audio.wav'),
    '-c:v', 'copy', '-c:a', 'aac', '-b:a', '160k', '-shortest', '-movflags', '+faststart', OUT,
  ], { stdio: 'inherit' });
  fs.rmSync(dir, { recursive: true, force: true });
  if (m.status !== 0) process.exit(m.status || 1);
  console.log('wrote ' + OUT);
})().catch((e) => { console.error(e); process.exit(1); });
