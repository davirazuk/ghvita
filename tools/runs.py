#!/usr/bin/env python3
"""What a chart asks of the fingers: its fastest stretches, in presses a second, and how fast the repeats are.
usage: runs.py notes.vhc part diff [--gap MS]"""
import argparse, struct, collections
ap = argparse.ArgumentParser()
ap.add_argument('vhc'); ap.add_argument('part', type=int); ap.add_argument('diff', type=int)
ap.add_argument('--gap', type=float, default=125)
a = ap.parse_args()
d = open(a.vhc, 'rb').read()
ver, ntempo, nbeats, nsec, ntracks, length = struct.unpack_from('<IIIIII', d, 4)
o = 28 + 8 * ntempo + 8 * nbeats + 64 * nsec
secs = [(struct.unpack_from('<i', d, 28 + 8 * ntempo + 8 * nbeats + 64 * i)[0] / 1e6,
         d[28 + 8 * ntempo + 8 * nbeats + 64 * i + 4:28 + 8 * ntempo + 8 * nbeats + 64 * i + 64].split(b'\0')[0].decode('utf8', 'replace'))
        for i in range(nsec)]
notes = None
for t in range(ntracks):
    pid, di, _, nn, nsp, nsolo = struct.unpack_from('<BBHIII', d, o); o += 16
    ns = [struct.unpack_from('<iIBBH', d, o + 12 * i) for i in range(nn)]; o += 12 * nn + 8 * nsp + 8 * nsolo
    if (pid, di) == (a.part, a.diff):
        notes = ns
assert notes, 'no such part'
names = 'GRYBOo'
fr = lambda f: ''.join(names[i] for i in range(6) if f >> i & 1) or '?'
gaps = [(notes[i + 1][0] - notes[i][0]) / 1000 for i in range(len(notes) - 1)]
print('notes', len(notes), 'version', ver)
h = collections.Counter(int(g // 10 * 10) for g in gaps)
print('gaps ms:', ' '.join('%d:%d' % (k, h[k]) for k in sorted(h)[:18]))
fl = collections.Counter((n[3] & 3) for n in notes)
print('strum/hopo/tap', fl[0], fl[1], fl[2], 'chords', sum(1 for n in notes if bin(n[2] & 63).count('1') > 1))
i = 0
while i < len(notes) - 1:
    if gaps[i] > a.gap:
        i += 1
        continue
    j = i
    while j < len(gaps) and gaps[j] <= a.gap:
        j += 1
    run = notes[i:j + 1]
    if len(run) >= 5:
        kinds = collections.Counter(fr(n[2]) for n in run)
        t = run[0][0] / 1e6
        sec = [s[1] for s in secs if s[0] <= t][-1:] or ['']
        rate = (len(run) - 1) / ((run[-1][0] - run[0][0]) / 1e6)
        hop = sum(1 for n in run if n[3] & 3)
        print('%7.2f %3d notes %5.1f/s hopo %3d  %-18s %s' % (t, len(run), rate, hop, sec[0][:18],
              ' '.join('%s:%d' % k for k in kinds.most_common(5))))
    i = j + 1
