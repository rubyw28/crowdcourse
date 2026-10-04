#!/usr/bin/env python3
"""Build the demo soundtrack from the cue timeline that demo/record.js captures.

    python3 demo/make_audio.py timeline.json out.wav

Everything is synthesized here (no samples, no downloads): a music track that follows
the story's sections, plus effects tied to what happens on screen.
Cue times in timeline.json are seconds from the first video frame.
"""
import array
import json
import math
import random
import sys
import wave

SR = 44100
BPM = 112
BEAT = 60 / BPM
STEP = BEAT / 4            # sixteenth note
BAR = BEAT * 4

timeline_path, out_path = sys.argv[1:3]
timeline = json.load(open(timeline_path))
duration = timeline["duration"]
N = int((duration + 0.5) * SR)
music = array.array("f", bytes(4 * N))
sfx = array.array("f", bytes(4 * N))
rng = random.Random(7)


def mtof(m):
    return 440.0 * 2 ** ((m - 69) / 12)


def add(buf, samples, t, gain=1.0):
    i0 = int(t * SR)
    for k in range(max(0, min(len(samples), N - i0))):
        buf[i0 + k] += samples[k] * gain


# ---------- instruments ----------
def pad(freqs, secs, attack=0.5, release=0.8):
    n = int((secs + release) * SR)
    out = [0.0] * n
    for f in freqs:
        for detune in (0.997, 1.003):
            w = 2 * math.pi * f * detune / SR
            ph = rng.random() * 6.28
            for i in range(n):
                out[i] += math.sin(ph + w * i)
    scale = 1 / (2 * len(freqs))
    for i in range(n):
        t = i / SR
        env = min(1.0, t / attack) if t < secs else max(0.0, 1 - (t - secs) / release)
        out[i] *= env * scale
    return out


def pluck(f, secs=0.45):
    n = int(secs * SR)
    w = 2 * math.pi * f / SR
    return [(math.sin(w * i) + 0.25 * math.sin(3 * w * i)) * math.exp(-i / SR * 9) * min(1, i / 60)
            for i in range(n)]


_bass_cache = {}


def bass(f, secs):
    key = (round(f, 2), round(secs, 3))
    if key not in _bass_cache:
        _bass_cache[key] = _bass(f, secs)
    return _bass_cache[key]


def _bass(f, secs):
    n = int((secs + 0.05) * SR)
    w = 2 * math.pi * f / SR
    out, y, a = [], 0.0, 0.06                     # one-pole low-pass keeps it round
    for i in range(n):
        x = sum(math.sin(k * w * i) / k for k in range(1, 7))
        y += a * (x - y)
        t = i / SR
        env = min(1, t / 0.006) * (0.55 + 0.45 * math.exp(-t * 6)) * (1 if t < secs else max(0, 1 - (t - secs) / 0.05))
        out.append(y * env * 0.5)
    return out


def kick():
    n = int(0.32 * SR)
    out, ph = [], 0.0
    for i in range(n):
        t = i / SR
        ph += 2 * math.pi * (45 + 85 * math.exp(-t * 30)) / SR
        out.append(math.sin(ph) * math.exp(-t * 11))
    return out


def hat():
    n = int(0.045 * SR)
    prev, out = 0.0, []
    for i in range(n):
        x = rng.uniform(-1, 1)
        out.append((x - prev) * math.exp(-i / SR * 90) * 0.5)   # differencing = crude high-pass
        prev = x
    return out


KICK, HAT = kick(), hat()
PLUCK_CACHE = {}

# chords: (bass midi, pad midis); A minor, i - VI - III - VII
PROG = [(45, (57, 60, 64, 71)), (41, (53, 57, 60, 64)), (48, (55, 60, 64, 71)), (43, (55, 59, 62, 69))]
TENSE = [(45, (57, 60, 64, 69)), (40, (56, 59, 64, 68))]           # Am / E: the SOS scene

# which layers play in each section of the film
SECTIONS = {
    "title": dict(pad=0.55, arp=0.25, arp_rate=2),
    "s0":    dict(pad=0.45, arp=0.35, arp_rate=1, bass=0.5, kick=0.55, hat=0.18),   # hotter, colder
    "s1":    dict(pad=0.50, arp=0.30, arp_rate=4, bass=0.35, bass_long=True),     # out of range: sparse, searching
    "s2":    dict(pad=0.40, bass=0.55, bass_8ths=True, kick=0.6, hat=0.12, tense=True),  # friend SOS
    "s3":    dict(pad=0.45, arp=0.35, arp_rate=1, bass=0.5, kick=0.55, hat=0.2),   # own SOS
    "s4":    dict(pad=0.45, arp=0.30, arp_rate=2, bass=0.4, hat=0.14),             # calibration
    "end":   dict(pad=0.60, arp=0.30, arp_rate=1, bass=0.5, kick=0.5, hat=0.16),
}

marks = sorted((e["t"], e["id"]) for e in timeline["events"] if e["kind"] == "section")
end_at = next((t for t, sid in marks if sid == "end"), duration)


def section_at(t):
    cur = "title"
    for mt, sid in marks:
        if mt <= t + STEP / 2:
            cur = sid
    return cur


# ---------- compose: one bar at a time ----------
bar_i, t = 0, 0.0
final_at = end_at + BAR                         # one full bar of the ending, then a held chord
while t < final_at and t < duration:
    sec = SECTIONS[section_at(t)]
    prog = TENSE if sec.get("tense") else PROG
    root, chord = prog[bar_i % len(prog)]
    if sec.get("pad"):
        add(music, pad([mtof(m) for m in chord], BAR), t, sec["pad"])
    if sec.get("bass"):
        if sec.get("bass_long"):
            add(music, bass(mtof(root), BAR * 0.9), t, sec["bass"])
        else:
            hits = range(8) if sec.get("bass_8ths") else (0, 3, 4, 6)
            for h in hits:
                add(music, bass(mtof(root), BEAT / 2 * 0.85), t + h * BEAT / 2, sec["bass"] * (1 if h % 4 == 0 else 0.75))
    for s in range(16):
        ts = t + s * STEP
        sec_s = SECTIONS[section_at(ts)]
        if sec_s.get("kick") and s % 4 == 0:
            add(music, KICK, ts, sec_s["kick"] * (1 if s % 8 == 0 else 0.8))
        if sec_s.get("hat") and s % 2 == 1:
            add(music, HAT, ts, sec_s["hat"] * (1 if s % 4 == 2 else 0.7))
        if sec_s.get("arp") and s % sec_s.get("arp_rate", 1) == 0:
            tones = list(chord) + [m + 12 for m in chord]
            m = tones[(s * 3 + bar_i) % len(tones)]
            if m not in PLUCK_CACHE:
                PLUCK_CACHE[m] = pluck(mtof(m + 12))
            add(music, PLUCK_CACHE[m], ts, sec_s["arp"])
            add(music, PLUCK_CACHE[m], ts + 3 * STEP, sec_s["arp"] * 0.3)    # dotted-eighth echo
    t += BAR
    bar_i += 1

# held final chord that rings out under the end card
if t < duration:
    hold = max(1.0, duration - t - 1.0)
    add(music, pad([mtof(m) for m in (57, 60, 64, 71, 76)], hold, attack=0.2, release=1.0), t, 0.7)
    add(music, bass(mtof(45), hold), t, 0.45)

# ---------- effects, from the on-screen cues ----------
def tone(freq, secs, decay=0.0, square=False):
    n = int(secs * SR)
    w = 2 * math.pi * freq / SR
    out = []
    for i in range(n):
        s = math.sin(w * i)
        if square:
            s = 0.6 if s >= 0 else -0.6
        out.append(s * min(1, i / 180) * (math.exp(-i / SR * decay) if decay else 1) * min(1, (n - i) / 400))
    return out


def sweep(f0, f1, secs):
    out, ph, n = [], 0.0, int(secs * SR)
    for i in range(n):
        p = i / n
        ph += 2 * math.pi * f0 * (f1 / f0) ** p / SR
        out.append(math.sin(ph) * min(1, i / 400) * (0.4 + 0.6 * p))
    return out


TICK = tone(760, 0.07, decay=55)
TAP = tone(2200, 0.03, decay=160)
ALARM = tone(1040, 0.2, decay=4, square=True)
CHIME = tone(880, 0.16, decay=14) + tone(1320, 0.45, decay=7)

alarm_on = None
for e in timeline["events"]:
    t, kind = e["t"], e["kind"]
    if kind == "tick":
        add(sfx, TICK, t, 0.05 + 0.08 * e.get("heat", 0.5))
    elif kind == "tap":
        add(sfx, TAP, t, 0.2)
    elif kind == "hold":
        add(sfx, sweep(330, 990, e["ms"] / 1000), t, 0.12)
    elif kind == "alarm_on":
        alarm_on = t
    elif kind == "alarm_off" and alarm_on is not None:
        k = alarm_on
        while k < t:                                # same rhythm as the page: two pulses every 1.6 s
            add(sfx, ALARM, k, 0.06)
            add(sfx, ALARM, k + 0.3, 0.06)
            k += 1.6
        alarm_on = None
    elif kind == "measure":
        steps = int(e["ms"] / 500)
        for k in range(steps):
            add(sfx, tone(620 * 2 ** (k / (steps * 2)), 0.05, decay=60), t + k * 0.5, 0.08)
    elif kind == "chime":
        add(sfx, CHIME, t, 0.2)

# ---------- mix, fade, normalise, write ----------
mpeak = max(1e-9, max(abs(s) for s in music))
out = array.array("f", bytes(4 * N))
for i in range(N):
    tt = i / SR
    fade = min(1.0, tt / 1.0, max(0.0, (duration - tt) / 2.5))
    out[i] = (music[i] / mpeak * 0.55 + sfx[i]) * fade
# gentle soft-clip so kick transients don't hold the whole mix down
pre = 1.0 / max(1e-9, max(abs(s) for s in out))
for i in range(N):
    out[i] = math.tanh(out[i] * pre * 1.3)
peak = max(1e-9, max(abs(s) for s in out))
g = 0.89 / peak
pcm = array.array("h", (int(max(-1.0, min(1.0, s * g)) * 32767) for s in out))
with wave.open(out_path, "wb") as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(SR)
    w.writeframes(pcm.tobytes())
print(f"soundtrack: {duration:.1f}s, {len(marks)} sections, {len(timeline['events'])} cues -> {out_path}")
