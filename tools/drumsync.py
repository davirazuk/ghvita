#!/usr/bin/env python3
"""How well a song's drum chart lines up with its audio: drumsync.py SONG_FOLDER [part] [diff]
Cross-correlates the gems with the onsets of the drum stems (each kit piece on its own where the song has them).
A positive lag means the sound comes after the gem."""
import glob, os, struct, subprocess, sys
import numpy as np

RATE = 16000
HOP = 16                # 1 ms


def track(vhc, part, diff):
    d = open(vhc, 'rb').read()
    ver, ntempo, nbeats, nsec, ntracks, length = struct.unpack_from('<IIIIII', d, 4)
    o = 28 + 8 * ntempo + 8 * nbeats + 64 * nsec
    for t in range(ntracks):
        pid, di, _, nn, nsp, nsolo = struct.unpack_from('<BBHIII', d, o)
        o += 16
        notes = [struct.unpack_from('<iIBBH', d, o + 12 * i) for i in range(nn)]
        o += 12 * nn + 8 * nsp + 8 * nsolo
        if (pid, di) == (part, diff):
            return notes
    return []


def onsets(path):
    pcm = subprocess.run(['ffmpeg', '-v', 'error', '-i', path, '-ac', '1', '-ar', str(RATE), '-f', 's16le', '-'],
                         capture_output=True).stdout
    x = np.frombuffer(pcm, '<i2').astype(np.float32) / 32768
    if not len(x):
        return np.zeros(1)
    x = x - np.convolve(x, np.ones(64) / 64, 'same')        # above the rumble, so a hit's front edge shows
    e = np.abs(x[:len(x) // HOP * HOP]).reshape(-1, HOP).max(1)
    e = np.log1p(e * 200)
    env = np.convolve(e, np.ones(6) / 6, 'same')
    d = np.diff(env, prepend=0)
    return np.maximum(d, 0)


def lag(times, on, span=0.3):
    n = len(on)
    train = np.zeros(n)
    k = (np.array(times) * 1000).astype(int)
    k = k[(k >= 0) & (k < n)]
    if not len(k):
        return None
    train[k] = 1
    train = np.convolve(train, np.ones(5) / 5, 'same')
    m = 1 << int(np.ceil(np.log2(2 * n)))
    c = np.fft.irfft(np.fft.rfft(on, m) * np.conj(np.fft.rfft(train, m)), m)
    w = int(span * 1000)
    win = np.r_[c[-w:], c[:w + 1]]
    best = int(np.argmax(win)) - w
    return best, float(win.max() / (np.median(win) + 1e-9)), len(k)


def main():
    folder = sys.argv[1]
    part, diff = (int(sys.argv[2]), int(sys.argv[3])) if len(sys.argv) > 3 else (5, 3)
    notes = track(os.path.join(folder, 'notes.vhc'), part, diff)
    print(os.path.basename(folder.rstrip('/')), len(notes), 'notes')
    pick = {'all': 0xff, 'kick': 0xa0, 'snare': 1, 'yellow': 2, 'blue': 4, 'green': 8}
    stems = sorted(glob.glob(glob.escape(folder) + '/drums*')) or sorted(glob.glob(glob.escape(folder) + '/song.*'))
    sums = None
    for s in stems:
        on = onsets(s)
        sums = on if sums is None else sums[:len(on)] + on[:len(sums)]
        row = []
        for name, m in pick.items():
            r = lag([n[0] / 1e6 for n in notes if n[2] & m], on)
            if r:
                row.append('%s %+dms x%.1f' % (name, r[0], r[1]))
        print('  %-14s %s' % (os.path.basename(s), '  '.join(row)))


main()
