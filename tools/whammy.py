#!/usr/bin/env python3
"""Reads what test/tools/whammy_main.c made. whammy.py tones DIR | stem IN OUT [--bent]"""
import sys
import numpy as np

R = 2 ** (-1 / 12)

def load(p):
    return np.fromfile(p, dtype=np.int16).reshape(-1, 2).astype(np.float64)

def peak_hz(x, near, span=0.12):
    """the strongest frequency near a target, to a fraction of a bin: 8 s of a tone gives 0.125 Hz bins"""
    w = np.hanning(len(x)); s = np.abs(np.fft.rfft(x * w)); hz = np.fft.rfftfreq(len(x), 1 / 48000)
    m = (hz > near * (1 - span)) & (hz < near * (1 + span))
    i = np.where(m)[0][np.argmax(s[m])]
    a, b, c = np.log(s[i - 1:i + 2] + 1e-9)
    return hz[i] + 0.5 * (a - c) / (a - 2 * b + c) * (hz[1] - hz[0]), s[i]

def spec(x):
    w = np.hanning(4096)
    return np.array([np.abs(np.fft.rfft(x[i:i + 4096] * w)) for i in range(0, len(x) - 4096, 2048)])

def lag_ms(g, y):
    e = lambda x: np.abs(x[:len(x) // 48 * 48]).reshape(-1, 48).mean(1)
    a, b = e(g), e(y); n = min(len(a), len(b)); a = a[:n] - a[:n].mean(); b = b[:n] - b[:n].mean()
    return int(np.argmax([np.dot(a[30:-30], np.roll(b, -l)[30:-30]) for l in range(-20, 21)])) - 20

def clicks(x):
    """the worst jump a 5 ms stretch has over the 50 ms round it: what an ear hears as a click, not loud playing"""
    d = np.abs(np.diff(x)); n = len(d) // 240; m = d[:n * 240].reshape(-1, 240).max(1)
    around = np.convolve(m, np.ones(11) / 11, 'same')
    return float((m[10:-10] / (around[10:-10] + 8)).max()), float(m.max())

if sys.argv[1] == 'dips':
    # 5 ms steps of what came out against the stem: how far under it it ever falls, while the stem is sounding
    g, y = load(sys.argv[2])[:, 0], load(sys.argv[3])[:, 0]
    n = min(len(g), len(y)) // 240 * 240
    e = lambda x: np.sqrt((x[:n].reshape(-1, 240) ** 2).mean(1))
    a, b = e(y)[20:-20], e(g)[20:-20]
    m = b > max(200, np.median(b) * 0.5); r = 20 * np.log10((a[m] + 1) / (b[m] + 1))
    print('5 ms steps %d: worst %.1f dB, under -6 dB %d, under -12 dB %d' % (m.sum(), r.min(), (r < -6).sum(), (r < -12).sum()))
    sys.exit()
if sys.argv[1] == 'stem':
    g, y = load(sys.argv[2])[:, 0], load(sys.argv[3])[:, 0]
    n = min(len(g), len(y)) - 9600; g, y = g[4800:n], y[4800:n]
    lvl = 10 * np.log10((y ** 2).sum() / (g ** 2).sum())
    s = np.log10(spec(g) + 1), np.log10(spec(y) + 1)
    low = lambda a: (a[:, 1:14] ** 2)
    sg, sy = spec(g), spec(y)
    lo = 10 * np.log10((sy[:, 1:14] ** 2).sum() / (sg[:, 1:14] ** 2).sum())
    idx = np.arange(sg.shape[1]); moved = np.array([np.interp(idx, idx * R, r) for r in sg])
    db = lambda a: 20 * np.log10(a + 1)
    dist = np.mean(np.abs(db(sy[:, 4:800]) - db(moved[:, 4:800])))
    print('level %+.2f dB, below 300 Hz %+.2f dB, %.2f dB from ideal, lag %d ms, click %.1f/%.1f (bent/stem), worst step %.0f/%.0f'
          % (lvl, lo, dist, lag_ms(g, y), clicks(y)[0], clicks(g)[0], clicks(y)[1], clicks(g)[1]))
