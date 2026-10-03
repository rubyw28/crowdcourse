# Crowdsource

Bracelets that help friends find each other when a crowd knocks out the usual tools: a dead battery, a jammed cell network, or a GPS dot that cannot tell "beside you" from "across the room." Each ESP32 listens for its friend's ESP-NOW radio signal (RSSI) and buzzes faster as they get closer, so you can keep your eyes on the people around you. Either friend can send an SOS, and it stays on until the other person acknowledges it. A phone joins its bracelet's Wi-Fi hotspot and opens a page the bracelet serves, which shows a 0–100 closeness score, the SOS controls and a full-screen alert. No internet, no account, and no app install. A borrowed phone is enough.

The people this is for are the ones a crowd separates: a friend who can't look down at a map, someone whose phone died, a pair trying to leave a show together. The band is the search. The laptop dashboard is only for whoever is trying to help them.

The closeness score is fit to this pair, not to a universal radio curve. Calibration records how the two bracelets sound when they stand together and when they step apart. After that, a slow change means someone is walking closer or farther. A sharp drop, while the beacons keep arriving, is scored as a person stepping between them, and the number holds instead of telling you your friend left.

On top of that offline core, a bracelet plugged into a laptop streams everything it hears into a Tiger Data (Postgres + TimescaleDB) database. The online features read from it: an organizer dashboard, an iMessage agent and the sponsor integrations, which are in progress.

```
bracelet B ))) bracelet A ──USB──> laptop bridge ──> Tiger Data ──> dashboard, iMessage agent, ...
                    │
                 phone (bracelet's own Wi-Fi, offline)
```

```
index.html                    the whole phone/desktop UI in one file (no network requests, ~37 KB)
mock-server.js                fake bracelet for working on the UI without hardware (zero dependencies)
firmware/crowdsource/         bracelet firmware: ESP-NOW + hotspot + web page + USB telemetry (flash to both)
firmware/crowdsource_test/    one-board firmware with a simulated friend, for testing the page on real phones
firmware/embed_html.py        packs index.html into both sketches (run after editing index.html)
bridge/                       laptop bridge (USB serial -> Tiger Data) and the database schema
lib/                          shared Node helpers: .env loader, Tiger Data connection (+ its CA certificate)
.env.example                  every key the project uses; copy to .env (git-ignored)
crowd_source/                 3-friend bracelet sketch (LED strip, 16x2 LCD, buzzer, Lighthouse) + the same phone page
demo/                         scripted walkthrough: demo.mp4, the page it's recorded from, and the recorder
```

## Try the UI without hardware

Needs Node 18+ ([nodejs.org](https://nodejs.org)); the mock has no dependencies.

```sh
node mock-server.js            # http://localhost:8080
PORT=9000 FRIEND=Sam SOS_EVERY=30 node mock-server.js
```

Open the printed LAN address on a phone on the same Wi-Fi to try the phone layout. While it runs, press `s` for a friend SOS, `c` to end it, `n` / `f` to make the friend walk close or far, `l` to cut the signal, and `q` to quit. The terminal logs everything the page sends.

## Run it on the bracelets

**Setup (once):** in the Arduino IDE, install the **esp32** boards package by Espressif (3.x) and the libraries **ESP Async WebServer** and **Async TCP** (both by ESP32Async). Use the board **ESP32 Dev Module**.

1. If you changed `index.html`, run `python3 firmware/embed_html.py`.
2. Flash `firmware/crowdsource/crowdsource.ino` to **both** boards. No per-board changes are needed: each one names itself from its chip ID.
3. Power both boards. Within a second, each Serial Monitor (115200) shows `paired with Band XXXX`.
4. On each phone, join that bracelet's Wi-Fi (`Crowdsource-XXXX`, no password) and open `http://192.168.4.1`. Stay connected when the phone warns there's no internet.
5. Calibrate in the room you're in: stand together, then step apart.

With no extra wiring, the **BOOT button** is the SOS button (hold 1.5 s to start or end) and the **onboard blue LED** stands in for the vibration motor. Change `BUTTON_PIN`, `HAPTIC_PIN` and `BATTERY_PIN` at the top of the sketch for real parts. Serial commands for testing without phones: `s` start/end SOS, `a` acknowledge the friend's SOS, `p` forget the friend and pair again.

To check the page on real phones with a single board, flash `firmware/crowdsource_test/` instead. It fakes the friend and accepts the same keys as the mock over serial.

### How the bracelets talk

Each bracelet broadcasts a 28-byte beacon 10 times a second on Wi-Fi channel 6: its name, battery, an SOS flag, its SOS sequence number, and the last friend SOS number it acknowledged. The first bracelet heard with the same `GROUP_ID` becomes the friend. Because the SOS state rides in every beacon, a dropped packet can't lose an alert or an acknowledgment. Sequence numbers start at a random value on boot, so a restarted bracelet's new SOS is never mistaken for one that was already acknowledged.

`crowd_source/` is a separate design (NeoPixel strip, 16x2 LCD, buzzer, up to 3 friends, "Lighthouse" mode) with its own packet format on channel 1. It doesn't talk to `firmware/crowdsource/`. It serves the same phone page from a hotspot named `Crowdsource-<NAME>`: the page shows whichever friend SELECT is tracking, its SOS turns on Lighthouse, and acknowledging a friend's Lighthouse on the phone tells their bracelet help is coming. It also prints the same `@{...}` USB telemetry, so the bridge below works with it (bracelets are named by `NAMES`, e.g. `Ruby`).

## Stream bracelet data to Tiger Data

1. **Create the database.** With the [Tiger CLI](https://github.com/timescale/tiger-cli):
   ```sh
   curl -fsSL https://cli.tigerdata.com | sh
   tiger auth login
   tiger service create --name crowdsource --cpu shared     # free tier
   echo "DATABASE_URL=$(tiger db connection-string --with-password)" >> .env
   ```
   Or create a service in the [Tiger Console](https://console.cloud.tigerdata.com) and paste its connection string into `.env` as `DATABASE_URL`. Treat it like a password.
2. **Install and run the bridge** with a bracelet plugged into USB:
   ```sh
   npm install
   npm run bridge                          # finds the bracelet's port by itself
   npm run bridge -- --port /dev/cu.usbserial-3 --verbose
   ```
   The bridge creates the tables on first run. Without `DATABASE_URL` it does a dry run and only prints what it would store. To test without hardware, pipe saved telemetry into `node bridge/bridge.js --stdin`.

**What gets stored** ([bridge/schema.sql](bridge/schema.sql)):

| Table | Contents |
|---|---|
| `readings` (hypertable) | Every beacon heard: `time`, `bracelet`, `friend`, `rssi`, about 10 rows per second per pair. |
| `events` (hypertable) | `paired`, `lost` / `found`, `my_sos`, `my_sos_acked`, `my_sos_end`, `friend_sos`, `friend_sos_acked`, `friend_sos_end`, `phone`, `calibrate`, with extra fields in `detail` (jsonb). |
| `readings_10s` (continuous aggregate) | Average RSSI and packet count per 10 s, refreshed by TimescaleDB every 10 s. The dashboard charts from this. |

## Organizer dashboard

With `DATABASE_URL` in `.env`:

```sh
npm run web            # http://localhost:8787
```

The page charts closeness from the `readings_10s` continuous aggregate, lists incidents, and times how long a pair took to get from far back to very close. These read Tiger Data only. Finding a friend and sending SOS still work on the bracelet with the laptop closed.

## Aid station (Raspberry Pi)

The Pi is the table at the edge of the crowd: the place a friend, or event staff, watches who is apart and who raised an SOS. It does not find anyone. The bracelets still do that over ESP-NOW, with no Pi and no internet.

Put the project on the Pi (Node 20 or newer), copy `.env` onto it, and install there so the serial library builds for that machine:

```sh
npm install
npm run station          # bridge + dashboard, http://<pi-address>:8787
```

Plug **one** bracelet into the Pi's USB port and leave it there. The other bracelet is the one that walks. On the Pi, your user needs access to the serial port (`sudo usermod -aG dialout $USER`, then log out and back in). The port is usually `/dev/ttyUSB0`.

Open the dashboard from any phone on the **same network as the Pi**. A phone joined to a bracelet's hotspot cannot see the Pi. If the venue Wi-Fi blocks device-to-device traffic, have the Pi and the helper's phone share a phone hotspot instead.

If the station bracelet also has a camera on a 64-bit Pi, the Presage check-in at `/checkin` can run there: `npm install @smartspectra/node-sdk` and set `PRESAGE_API_KEY`. Video stays on the Pi. Only the pulse and breathing numbers are stored.

The same server turns on the sponsor pieces when the matching key is in `.env` (names are in `.env.example`):

| Route | Needs |
|---|---|
| Ask box, and iMessage replies | `GEMINI_API_KEY` |
| `POST /api/photon` | `npm install spectrum-ts`, then Photon project id, secret, and webhook secret. Point Spectrum at `https://<your tunnel>/api/photon` |
| Speak with ElevenLabs / Speak with Grok | `ELEVENLABS_API_KEY` / `XAI_API_KEY` |
| Award Crowd Hero | nothing (Solana devnet; wallet is created in `.keys/`) |
| Responder check-in at `/checkin` | `PRESAGE_API_KEY` and `npm install @smartspectra/node-sdk` |

An SOS event (`my_sos`) texts `EMERGENCY_CONTACTS` when Photon is configured. Register the public site with GoDaddy Registry using code `MLHBRH2699`, then serve this process behind that domain.

The firmware sends telemetry as one JSON object per serial line, prefixed with `@` so it can share the port with the human-readable log:

```
@{"ev":"rssi","me":"Band 94C1","friend":"Band 4661","rssi":-55}
@{"ev":"friend_sos","me":"Band 94C1","friend":"Band 4661","seq":47021}
```

Tiger Data signs connections with its own certificate authority (`ca.timescale.com`). `lib/db.js` verifies against the copy in `lib/tiger-ca.pem`, so connections stay encrypted and checked; don't switch verification off. Opening the serial port doesn't reset the bracelet, and the port name can change when you replug it, so let the bridge find it.

## WebSocket contract (`/ws`, JSON text frames)

The page connects to `ws://<host that served it>/ws`, which is `ws://192.168.4.1/ws` on a bracelet. You can override it in Settings (saved on the phone) or with `?ws=ws://host/ws`.

### Bracelet → phone

| Message | When |
|---|---|
| `{"type":"rssi","rssi":-63}` | Every time a packet from the friend arrives (2–10 Hz is ideal). |
| `{"type":"status","name":"Alex","battery":92,"friendBattery":67}` | **At least every 2 s.** This is also the heartbeat: if the phone hears nothing for 6 s, it reconnects. `name` is the friend's name. `battery` (this bracelet) and `friendBattery` are optional. |
| `{"type":"sos"}` | The friend started an SOS. Also sent again to a phone that connects while it's unacknowledged. |
| `{"type":"sos_clear"}` | The friend ended their SOS. |
| `{"type":"sos_ack"}` | The friend acknowledged *our* SOS. |

### Phone → bracelet

| Message | When |
|---|---|
| `{"type":"hello"}` | On every (re)connect. |
| `{"type":"calibrate","near":-45,"far":-85}` | After calibrating, and again on every reconnect, so the haptics use the same thresholds as the page. |
| `{"type":"sos"}` | The user held SOS for 1.5 s. If the socket is down, it's queued and sent on reconnect. |
| `{"type":"sos_cancel"}` | The user ended their SOS. |
| `{"type":"sos_ack"}` | The user acknowledged the friend's SOS. |

## RSSI → closeness score

1. **Smooth:** take the mean of the last 10 samples, dropping any older than 3 s.
2. **Score:** `100 × (avg − far) / (near − far)`, clamped to 0–100. The defaults are near −45 dBm and far −85 dBm. RSSI is already logarithmic in distance, so this linear map spends most of its range on the last few metres, where you actually need it.
3. **Label:** ≥70 very close, ≥40 nearby, otherwise far, with ±4 points of hysteresis so the label doesn't flicker. No RSSI for 5 s shows *lost*.
4. **Pulse:** the period runs from 1.8 s at score 0 down to 0.35 s at score 100. The bracelet's haptics use the same curve.
5. **Calibrate:** take the median of 5 s of raw samples standing together (near), then again standing apart (far). Far must be at least 8 dB below near.

## Demo

`demo/demo.mp4` is an 83-second walkthrough with music, recorded from the real `index.html`. To present it live instead, run the mock and open `http://localhost:8080/demo` full screen; reload to replay (the live version has no sound).

To re-record after changing the UI (needs Google Chrome):

```sh
npm i --no-save puppeteer-core ffmpeg-static
node demo/record.js            # records the video, then synthesizes the music and sound effects
```

The soundtrack (music sections and effects) is generated by `demo/make_audio.py` from cues the page logs while it plays.
