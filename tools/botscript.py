#!/usr/bin/env python3
"""A test player for the PC build: presses the buttons (layout A) for a chart's notes at their times, for GHV_SCRIPT.
usage: botscript.py notes.vhc part diff [--speed 5] [--miss N] [--late MS] [--overhit N] [--sp] [--shots F,F..]
--miss N skips every Nth note, --late shifts every press, --overhit N adds a stray press after every Nth note,
--sp presses Select whenever it can (the game ignores it until the meter is half full)"""
import argparse, struct
ap = argparse.ArgumentParser()
ap.add_argument('vhc'); ap.add_argument('part', type=int); ap.add_argument('diff', type=int)
ap.add_argument('--speed', type=int, default=5); ap.add_argument('--miss', type=int, default=0)
ap.add_argument('--late', type=float, default=0); ap.add_argument('--overhit', type=int, default=0)
ap.add_argument('--sp', action='store_true'); ap.add_argument('--shots', default='')
ap.add_argument('--limit', type=float, default=0, help='stop after this many seconds of song')
a = ap.parse_args()
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
LANE = ['l', 'left', 'up', 'circle', 'r', 'cross']           # layout A: G R Y B O, open
if a.part == 5:
    five = any(n[2] & 0x10 for n in notes)
    LANE = ['left', 'up', 'triangle', 'square' if five else 'circle', 'circle', 'cross']
Z_FAR = 11.0
v = 4 + a.speed * 0.8
first = notes[0][0] / 1e6
lead = max(1.2, Z_FAR / v + 0.6 - first)
frame = lambda t: int(round((t + lead + a.late / 1000) * 60))
# per lane, the frames it's held: (press, release)
holds = [[] for _ in range(6)]
def hold(l, f, end):
    if holds[l] and holds[l][-1][1] >= f:
        p, r = holds[l][-1]
        if p >= f - 1:
            return                          # too close to press again
        holds[l][-1] = (p, f - 1)           # let go just before
    holds[l].append((f, end))
taken = 0
for i, (t, ln, frets, flags, sus) in enumerate(notes):
    t /= 1e6
    if a.limit and t > a.limit:
        break
    if a.miss and i % a.miss == a.miss - 1:
        continue
    f = frame(t)
    lanes = [5] if frets & 0x20 and a.part != 5 else [l for l in range(5) if frets >> l & 1]
    if a.part == 5 and frets & 0xa0:
        lanes.append(5)                     # the kick, under the pads
    for l in lanes:
        hold(l, f, frame(t + ln / 1e6) if ln else f + 2)
    taken += 1
    if a.overhit and i % a.overhit == 0 and i + 1 < len(notes):
        nf = frame(notes[i + 1][0] / 1e6)
        busy = frets | notes[i + 1][2]
        free = [l for l in range(5) if not busy >> l & 1]
        if nf - f >= 30 and free and all(h[1] < f + 12 for h in holds[free[0]][-1:]):
            hold(free[0], f + 12, f + 14)
ev = {}
for l in range(6):
    for p, r in holds[l]:
        ev.setdefault(p, []).append('press ' + LANE[l])
        ev.setdefault(r, []).append('release ' + LANE[l])
if a.sp:
    for i, n in enumerate(notes[::8]):
        f = frame(n[0] / 1e6) + 3
        ev.setdefault(f, []).append('press select')
        ev.setdefault(f + 2, []).append('release select')
lines = []
for f in sorted(ev):
    for l in sorted(ev[f], key=lambda x: x.startswith('press')):     # releases first
        lines.append('%d %s' % (f, l))
for s in a.shots.split(','):
    if s:
        lines.append('%d shot %s' % (frame(float(s.split(':')[1])), s.split(':')[0]) if ':' in s else '')
last = max(ev) if ev else 0
end_t = (a.limit if a.limit else length / 1000 + 4)
lines.append('%d quit' % max(last + 60, frame(end_t)))
print('\n'.join(l for l in lines if l))
