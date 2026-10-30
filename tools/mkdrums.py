# the kit freestyle plays in a song's gaps, synthesised: assets/sfx/drum_*.ogg
import os, subprocess, sys
import numpy as np

RATE = 48000
rng = np.random.default_rng(7)


def times(secs):
    return np.arange(int(secs * RATE)) / RATE


def bandnoise(n, lo, hi):
    spec = np.fft.rfft(rng.standard_normal(n))
    f = np.fft.rfftfreq(n, 1 / RATE)
    spec[(f < lo) | (f > hi)] = 0
    x = np.fft.irfft(spec, n)
    return x / np.max(np.abs(x))


def tone(t, start, end, fall):
    freq = end + (start - end) * np.exp(-t * fall)
    return np.sin(2 * np.pi * np.cumsum(freq) / RATE)


def kick():
    t = times(0.35)
    click = bandnoise(len(t), 1500, 6000) * np.exp(-t * 300) * 0.35
    return tone(t, 150, 52, 32) * np.exp(-t * 11) + click


def snare():
    t = times(0.3)
    body = (tone(t, 260, 185, 40) + 0.5 * tone(t, 340, 330, 10)) * np.exp(-t * 26) * 0.6
    return body + bandnoise(len(t), 1800, 11000) * np.exp(-t * 17) * 0.85


def hat():
    t = times(0.14)
    return bandnoise(len(t), 7000, 18000) * np.exp(-t * 48)


def tom(f):
    t = times(0.5)
    return tone(t, f * 1.5, f, 22) * np.exp(-t * 9) + bandnoise(len(t), 400, 3000) * np.exp(-t * 90) * 0.2


def crash():
    t = times(1.3)
    return (bandnoise(len(t), 3000, 16000) * np.exp(-t * 4.2) +
            bandnoise(len(t), 600, 5000) * np.exp(-t * 10) * 0.35)


kit = {"kick": kick(), "snare": snare(), "hat": hat(), "tom_hi": tom(190), "tom_lo": tom(105), "crash": crash()}
out = sys.argv[1]
for name, x in kit.items():
    x = x / np.max(np.abs(x)) * 0.92
    x[-256:] *= np.linspace(1, 0, 256)
    pcm = (x * 32767).astype("<i2").tobytes()
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "s16le", "-ar", str(RATE), "-ac", "1", "-i", "-",
                    "-ac", "2", "-c:a", "libvorbis", "-q:a", "3", os.path.join(out, f"drum_{name}.ogg")],
                   input=pcm, check=True)
