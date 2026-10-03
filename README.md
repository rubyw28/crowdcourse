# Crowdsource

Two ESP32 bracelets find each other in a crowd using ESP-NOW signal strength (RSSI), with haptics and an SOS button. A phone joins one bracelet's Wi-Fi hotspot and opens the page that bracelet serves.

```
index.html       whole web UI in one file (no network needed); flash this to the ESP32
mock-server.js   fake bracelet for development, zero dependencies
```

## Run the mock

```sh
node mock-server.js            # http://localhost:8080
PORT=9000 FRIEND=Sam SOS_EVERY=30 node mock-server.js
```

Open the printed LAN URL on your phone (same Wi-Fi) to test on a real device. Keys while it runs: `s` friend sends SOS, `c` friend ends SOS, `n` walk close, `f` walk far, `l` toggle signal lost, `q` quit.

The page connects to `ws://<host that served it>/ws`, so it works unchanged on the ESP32 (`ws://192.168.4.1/ws`) and on the mock. You can override the address in Settings (it's saved on the phone) or with `?ws=ws://host/ws`.

## WebSocket contract (`/ws`, JSON text frames)

### Bracelet → phone

| Message | When |
|---|---|
| `{"type":"rssi","rssi":-63}` | Every time an ESP-NOW packet from the friend arrives (2–10 Hz is ideal). |
| `{"type":"status","name":"Alex","battery":92,"friendBattery":67}` | **At least every 2 s.** This is also the heartbeat: if the phone hears nothing for 6 s, it drops the socket and reconnects. `battery` is this bracelet and `friendBattery` is the friend's; both are optional. |
| `{"type":"sos"}` | The friend pressed SOS. |
| `{"type":"sos_clear"}` | The friend ended their SOS. |
| `{"type":"sos_ack"}` | The friend acknowledged *our* SOS. |

Any message may include an `rssi` field, which is used as a sample.

### Phone → bracelet

| Message | When |
|---|---|
| `{"type":"hello"}` | On every (re)connect. |
| `{"type":"calibrate","near":-45,"far":-85}` | After calibrating, and again on every reconnect, so the bracelet's haptics can use the same thresholds. |
| `{"type":"sos"}` | The user held SOS for 1.5 s. If the socket is down, it's queued and sent on reconnect. |
| `{"type":"sos_cancel"}` | The user ended their SOS. |
| `{"type":"sos_ack"}` | The user acknowledged the friend's SOS. |

## RSSI → closeness score

1. **Smooth:** take the mean of the last 10 samples, dropping any older than 3 s.
2. **Score:** `100 × (avg − far) / (near − far)`, clamped to 0–100. The defaults are near −45 dBm and far −85 dBm. RSSI is already logarithmic in distance, so this linear map spends most of its range on the last few metres, where you actually need it.
3. **Label:** ≥70 very close, ≥40 nearby, otherwise far, with ±4 points of hysteresis so the label doesn't flicker. No RSSI for 5 s shows *lost*.
4. **Pulse:** the period runs from 1.8 s at score 0 down to 0.35 s at score 100.
5. **Calibrate:** take the median of 5 s of raw samples standing together (near), then again standing apart (far). Far must be at least 8 dB below near.
