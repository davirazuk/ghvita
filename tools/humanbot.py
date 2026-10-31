#!/usr/bin/env python3
"""A human-ish test player for GHV_SCRIPT: real timing spread, rolled chords, fingers landing on the wrong fret and
correcting, presses a little early, bounced keys, and notes that just get missed.
usage: humanbot.py notes.vhc part diff [--seed N] [--spread MS] [--slip P] [--early P] [--bounce P] [--miss P]"""
import argparse, random, struct
ap = argparse.ArgumentParser()
ap.add_argument('vhc'); ap.add_argument('part', type=int); ap.add_argument('diff', type=int)
ap.add_argument('--speed', type=int, default=5); ap.add_argument('--seed', type=int, default=1)
ap.add_argument('--spread', type=float, default=28); ap.add_argument('--roll', type=float, default=14)
ap.add_argument('--slip', type=float, default=0.03); ap.add_argument('--early', type=float, default=0.02)
ap.add_argument('--bounce', type=float, default=0.02); ap.add_argument('--miss', type=float, default=0.02)
ap.add_argument('--mash', type=float, default=0, help='no looking at the notes: this many random presses a second')
ap.add_argument('--hold', action='store_true', help="tremolos: fingers on the frets but no more presses after the first")
ap.add_argument('--tap', type=float, default=0, help="the fastest one thumb goes, presses a second: tremolos are kept going at this")
a = ap.parse_args()
R = random.Random(a.seed)
d = open(a.vhc, 'rb').read()
ver, ntempo, nbeats, nsec, ntracks, length = struct.unpack_from('<IIIIII', d, 4)
o = 28 + 8 * ntempo + 8 * nbeats + 64 * nsec
notes = None
for t in range(ntracks):
    pid, di, _, nn, nsp, nsolo = struct.unpack_from('<BBHIII', d, o); o += 16
    ns = [struct.unpack_from('<iIBBH', d, o + 12 * i) for i in range(nn)]; o += 12 * nn + 8 * nsp + 8 * nsolo
    if (pid, di) == (a.part, a.diff):
        notes = ns
assert notes, 'no such part'
LANE = ['l', 'left', 'up', 'circle', 'r', 'cross']
v = 4 + a.speed * 0.8
lead = max(1.2, 11.0 / v + 0.6 - notes[0][0] / 1e6)
frame = lambda t: int(round((t + lead) * 60))
presses = [[] for _ in range(6)]
# what a thumb can't do: the gems after a run's first, unless their turn comes round
skip = set()
if a.tap:
    i = 0
    while i < len(notes):
        j = i
        while j + 1 < len(notes) and notes[j + 1][2] == notes[i][2] and (notes[j + 1][0] - notes[j][0]) / 1e6 <= 0.12:
            j += 1
        if j - i + 1 >= 4:
            due = notes[i][0] / 1e6
            for k in range(i + 1, j + 1):
                if a.hold or notes[k][0] / 1e6 < due + 1 / a.tap:
                    skip.add(k)
                else:
                    due = notes[k][0] / 1e6
        i = j + 1
if a.mash:
    t = notes[0][0] / 1e6
    while t < notes[-1][0] / 1e6:
        presses[R.randrange(5)].append((t, 0.04))
        t += 1 / a.mash
    notes = []
for i, (t, ln, frets, flags, sus) in enumerate(notes):
    t /= 1e6
    if i in skip or R.random() < a.miss:
        continue
    lanes = [5] if frets & 0x20 else [l for l in range(5) if frets >> l & 1]
    base = R.gauss(0, a.spread / 1000)
    if R.random() < a.early:
        base = -R.uniform(0.11, 0.17)
    if R.random() < a.slip:
        # the wrong finger first, then the right one
        wrong = [l for l in range(5) if not frets >> l & 1 and (not lanes or abs(l - lanes[0]) == 1)]
        if wrong:
            w = R.choice(wrong)
            presses[w].append((t + base - R.uniform(0.03, 0.07), 0.05))
    for l in lanes:
        at = t + base + R.gauss(0, a.roll / 1000)
        hold = ln / 1e6 if ln else R.uniform(0.04, 0.09)
        presses[l].append((at, hold))
        if R.random() < a.bounce:
            presses[l].append((at + hold + R.uniform(0.02, 0.05), 0.03))
ev = {}
for l in range(6):
    last_r = -1
    for at, hold in sorted(presses[l]):
        p = frame(at)
        r = max(p + 1, frame(at + hold))
        if p <= last_r + 1:
            p = last_r + 2
            if r <= p:
                r = p + 1
        ev.setdefault(p, []).append('press ' + LANE[l])
        ev.setdefault(r, []).append('release ' + LANE[l])
        last_r = r
out = []
for f in sorted(ev):
    for x in sorted(ev[f], key=lambda s: s.startswith('press')):
        out.append('%d %s' % (f, x))
out.append('%d quit' % (max(ev) + 300))
print('\n'.join(out))
