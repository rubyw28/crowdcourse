# Crowd Course

<p align="center"><img src="logo.webp" alt="Crowd Course" width="420"></p>

<p align="center"><b>Safety simplified. Always chart your course in a crowd.</b></p>

Crowd Course is for the moment a crowd separates two people and the phone stops being a way back. At a show, a campus night, or a packed exit, the cell network clogs, GPS cannot tell "beside you" from "across the room," and a dead battery ends the search. That hits hardest when someone cannot stand there staring at a map: they need their eyes on the crowd, their phone died, or they only have a borrowed one.

Each person wears an ESP32 bracelet. It listens for its friends over ESP-NOW and glows from blue to red as they get closer, so the search stays on the wrist. Either person can start Lighthouse, an SOS that strobes every friend's strip in their color, beeps them, and points their bracelets at whoever needs help. The phone joins that bracelet's Wi-Fi and opens a page the bracelet serves: a 0–100 closeness score, the SOS control, and a full-screen alert. No internet, no account, no app. A borrowed phone is enough.

The score is learned from this pair, in this room. Stand together, then step apart. A slow change means someone is walking. A sharp drop, while the beacons keep arriving, means a person stepped between them, and the number holds so nobody walks the wrong way.

One bracelet can stay plugged into a laptop or a Raspberry Pi: the aid station. That screen is for the friend or staff member trying to help: who is apart, how they came apart, who raised SOS. It also answers questions over iMessage. Finding each other never depends on it.

## Judging demo

1. Two people, two bracelets, each phone on its own bracelet's hotspot at `http://192.168.4.1`.
2. Calibrate together, then apart. Say that the band just learned this pair in this room.
3. Walk apart until the strip turns blue. Walk back until it turns red.
4. Have someone step between the bracelets. The page says to stay on that line, and the number holds.
5. Start Lighthouse and have the other person acknowledge it on their phone.
6. Text the aid station `WATCH`, then switch the walking bracelet off. The station texts that it stopped while close, so it was a band off or something in the way, not a friend walking away. Ask it "did Kate walk away?"

```
bracelet B ))) bracelet A ──USB──> aid station: bridge ──> Tiger Data ──> dashboard, Gemini agent, iMessage
                    │
                 phone (bracelet's own Wi-Fi, offline)
```

```
crowd_source/          the bracelet: sketch, its phone page (index.html), and embed_html.py, which packs the page into index_html.h
station.js             the aid station: runs the bridge and the dashboard together (npm run station)
bridge/                USB serial -> Tiger Data, and the database schema
web/                   dashboard server, Gemini agent, and the Photon iMessage line
lib/                   .env loader and the Tiger Data connection (+ its CA certificate)
logo.webp              the Crowd Course logo
```

## The bracelets

**Setup (once):** in the Arduino IDE, install the **esp32** boards package by Espressif (3.x) and the libraries **Adafruit NeoPixel**, **LiquidCrystal I2C** (Frank de Brabander), **ESP Async WebServer** and **Async TCP** (both by ESP32Async). Use the board **ESP32 Dev Module**.

1. If you changed `crowd_source/index.html`, run `python3 crowd_source/embed_html.py`.
2. Flash `crowd_source/crowd_source.ino` to each board with a **different `MY_ID`** (0, 1 or 2) at the top of the sketch. `NAMES` maps each id to a name, for example `Ruby`.
3. On each phone, join that bracelet's Wi-Fi (`Crowdsource-<NAME>`, no password) and open `http://192.168.4.1`. Stay connected when the phone warns there's no internet.
4. Calibrate in the room you're in: stand together, then step apart. This also retunes the LED colors and the LCD bars.

Hardware: a 12-LED NeoPixel strip on pin 26, a buzzer on 14, BEACON and SELECT buttons on 13 and 12 (to GND), and a 16x2 I2C LCD on SDA 32 / SCL 33.

| Control | Does |
|---|---|
| SELECT, short press | Track the next friend. The strip flashes that friend's color. |
| BEACON, short press | Start or end Lighthouse (auto-off after 60 s unless started from the phone). |
| BEACON, long press | Mute the tether alert for 2 minutes. |

Every bracelet broadcasts a 9-byte packet 10 times a second on Wi-Fi channel 1, which it shares with its hotspot: its id, a Lighthouse flag, a sequence number, and whose Lighthouse its phone acknowledged. If a friend stays far or lost for 8 seconds, the buzzer sounds a tether alert.

## Aid station

The station is the table at the edge of the crowd. It does not find anyone. The bracelets still do that over ESP-NOW, with no station and no internet.

Needs Node 20+. Copy `.env.example` to `.env` and fill in `DATABASE_URL`; every other feature turns on when its key is set.

```sh
npm install
npm run station          # bridge + dashboard, http://<address>:8787
```

Plug **one** bracelet into USB and leave it there. The other bracelet is the one that walks. On a Raspberry Pi, your user needs the serial port (`sudo usermod -aG dialout $USER`, then log back in); it is usually `/dev/ttyUSB0`. Open the dashboard from a phone on the **same network as the station**. A phone joined to a bracelet's hotspot cannot see it.

### Tiger Data

Create a service in the [Tiger Console](https://console.cloud.tigerdata.com) (or `tiger service create --name crowdcourse --cpu shared` with the [Tiger CLI](https://github.com/timescale/tiger-cli)) and put its connection string in `.env` as `DATABASE_URL`. The bridge and the dashboard both apply [bridge/schema.sql](bridge/schema.sql) on start. It is safe to run repeatedly.

| Object | What it holds |
|---|---|
| `readings` (hypertable) | Every beacon heard: `time`, `bracelet`, `friend`, `rssi`, about 10 rows per second per pair. Compressed to columnstore after a day, segmented by pair. |
| `events` (hypertable) | `lost` / `found`, `my_sos`, `my_sos_acked`, `my_sos_end`, `friend_sos`, `friend_sos_acked`, `friend_sos_end`, `calibrate`, with extra fields in `detail` (jsonb). |
| `readings_10s` (continuous aggregate) | Average RSSI and packet count per pair per 10 s, refreshed every 10 s. The chart and Gemini's history tool read this. |
| `messages` (hypertable) | Every iMessage to and from the station. |
| `watchers` | Who texted `WATCH`. |

The separations the dashboard lists are computed in SQL ([web/data.js](web/data.js)): window functions pair each `lost` with the next `found`, and a lateral join takes `last()` RSSI and the `regr_slope` of the 40 seconds before. A falling signal is someone walking out of range. A strong, flat one that stops is a band switched off or a body in the way. The dashboard sends its nine queries in parallel and shows how long Tiger Data took.

### Gemini

With `GEMINI_API_KEY`, questions from the dashboard's Ask box and from iMessage go to an agent ([web/answer.js](web/answer.js)). It reads the live snapshot, then decides what else to look up through four tools, each a query on Tiger Data: `closeness_history`, `separations`, `signal_stats` (median, spread, beacons per second) and `recent_events`. The dashboard shows which tools it used. The line the dashboard shows when the situation changes, which is also the body of an SOS text, is a single call with a JSON response schema. If a model is over quota it sits out for a minute, and if Gemini fails entirely, the readings answer on their own.

### Photon iMessage

With `PHOTON_PROJECT_ID` and `PHOTON_SECRET`, the station listens on Spectrum's message stream ([web/photon.js](web/photon.js)), so it needs no public URL. Text it:

| Text | Reply |
|---|---|
| `WATCH` | Alerts from now on: SOS, a bracelet dropping out of range (and how), and back in range after how long. |
| `STOP` | No more alerts. |
| anything else | The Gemini agent's answer, for example "where is Kate?" or "did she walk away?" |

Numbers in `EMERGENCY_CONTACTS` always get the alerts. A link at the edge of range can flap, so lost and found alerts go out at most once per pair every 30 s.

### Telemetry

The bracelet prints one JSON object per serial line at 115200 baud, prefixed with `@` so it can share the port with its human-readable log:

```
@{"ev":"rssi","me":"Ruby","friend":"Kate","rssi":-55}
@{"ev":"friend_sos","me":"Ruby","friend":"Kate"}
```

`npm run bridge` runs the bridge alone (`-- --port /dev/cu.usbserial-0001 --verbose` to pick the port and echo the log). Without `DATABASE_URL` it does a dry run. To test without hardware, pipe saved telemetry into `node bridge/bridge.js --stdin`. Tiger Data signs connections with its own certificate authority; `lib/db.js` verifies against `lib/tiger-ca.pem`, so connections stay encrypted and checked.

## WebSocket contract (`/ws`, JSON text frames)

The page connects to `ws://<host that served it>/ws`, which is `ws://192.168.4.1/ws` on a bracelet. You can override it in Settings (saved on the phone) or with `?ws=ws://host/ws`.

| Bracelet → phone | When |
|---|---|
| `{"type":"rssi","rssi":-63}` | Each packet from the friend being tracked. |
| `{"type":"status","name":"Kate"}` | Every second, and right away when SELECT changes friends. It is also the heartbeat: the page reconnects after 6 s of silence. `name` is the tracked friend. |
| `{"type":"sos"}` | The tracked friend started Lighthouse. Sent again to a phone that connects while it is unacknowledged. |
| `{"type":"sos_clear"}` | The friend ended it. |
| `{"type":"sos_ack"}` | A friend's phone acknowledged *our* Lighthouse. |

| Phone → bracelet | When |
|---|---|
| `{"type":"hello"}` | On every (re)connect. |
| `{"type":"calibrate","near":-45,"far":-85}` | After calibrating, and on every reconnect. |
| `{"type":"sos"}` / `{"type":"sos_cancel"}` | Start or end Lighthouse from the phone. |
| `{"type":"sos_ack"}` | Acknowledge the friend's Lighthouse. |

## RSSI → closeness score

1. **Smooth:** take the mean of the last 10 samples, dropping any older than 3 s.
2. **Score:** `100 × (avg − far) / (near − far)`, clamped to 0–100. The defaults are near −45 dBm and far −85 dBm. RSSI is already logarithmic in distance, so this linear map spends most of its range on the last few metres, where you actually need it.
3. **Label:** ≥70 very close, ≥40 nearby, otherwise far, with ±4 points of hysteresis so the label doesn't flicker. No RSSI for 5 s shows *lost*.
4. **Calibrate:** take the median of 5 s of raw samples standing together (near), then again standing apart (far). Far must be at least 8 dB below near.
