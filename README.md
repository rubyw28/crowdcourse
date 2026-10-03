# Crowdsource

Bracelets that help friends find each other in a crowd. Each ESP32 bracelet listens for its friend's ESP-NOW radio signal (RSSI) and buzzes faster as they get closer, and either friend can send an SOS. A phone joins its bracelet's Wi-Fi hotspot and opens a page the bracelet serves, which shows a 0–100 closeness score, the SOS controls and a full-screen alert. No internet or app install is needed.

```
index.html                    the whole phone/desktop UI in one file (no network requests, ~37 KB)
mock-server.js                fake bracelet for working on the UI without hardware (zero dependencies)
firmware/crowdsource/         bracelet firmware: ESP-NOW + hotspot + web page (flash to both bracelets)
firmware/crowdsource_test/    one-board firmware with a simulated friend, for testing the page on real phones
firmware/embed_html.py        packs index.html into both sketches (run after editing index.html)
crowd_source/                 standalone bracelet sketch (LED strip, OLED, buzzer, 3 friends; no phone)
demo/                         scripted walkthrough: demo.mp4, the page it's recorded from, and the recorder
```

## Try the UI without hardware

Needs Node 18+ ([nodejs.org](https://nodejs.org)); nothing to install.

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

`crowd_source/` is a separate design (NeoPixel strip, OLED, buzzer, up to 3 friends, "Lighthouse" mode) with its own packet format on channel 1. It doesn't talk to `firmware/crowdsource/` yet.

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
