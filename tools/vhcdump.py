#!/usr/bin/env python3
"""print a notes.vhc: usage vhcdump.py FILE [part diff [count]]"""
import struct, sys
d = open(sys.argv[1], 'rb').read()
assert d[:4] == b'VHC1'
ver, ntempo, nbeats, nsec, ntracks, length = struct.unpack_from('<IIIIII', d, 4)
o = 28
tempos = [struct.unpack_from('<iI', d, o + 8 * i) for i in range(ntempo)]; o += 8 * ntempo
beats = [struct.unpack_from('<iB3x', d, o + 8 * i) for i in range(nbeats)]; o += 8 * nbeats
secs = [struct.unpack_from('<i60s', d, o + 64 * i) for i in range(nsec)]; o += 64 * nsec
print('length %d ms, %d tempos (first %.1f bpm), %d beat lines, %d sections' % (length, ntempo, 60e6 / tempos[0][1], nbeats, nsec))
names = ['guitar', 'bass', 'rhythm', 'coop', 'keys']
want = (sys.argv[2], int(sys.argv[3])) if len(sys.argv) > 3 else None
for t in range(ntracks):
    pid, di, _, nn, nsp, nsolo = struct.unpack_from('<BBHIII', d, o); o += 16
    notes = [struct.unpack_from('<iIBBH', d, o + 12 * i) for i in range(nn)]; o += 12 * nn
    sp = [struct.unpack_from('<ii', d, o + 8 * i) for i in range(nsp)]; o += 8 * nsp
    solos = [struct.unpack_from('<ii', d, o + 8 * i) for i in range(nsolo)]; o += 8 * nsolo
    hopo = sum(1 for n in notes if n[3] & 1); tap = sum(1 for n in notes if n[3] & 2)
    opn = sum(1 for n in notes if n[2] & 0x20); sus = sum(1 for n in notes if n[1])
    chords = sum(1 for n in notes if bin(n[2]).count('1') > 1)
    print('%-6s %d: %4d notes (%d chords, %d hopo, %d tap, %d open, %d sustains), %d SP phrases, %d solos, first %.2f s' % (
        names[pid], di, nn, chords, hopo, tap, opn, sus, nsp, nsolo, notes[0][0] / 1e6 if notes else 0))
    if want and want == (names[pid], di):
        for n in notes[:int(sys.argv[4]) if len(sys.argv) > 4 else 40]:
            print('   %9.3f len %6.3f %s%s %s' % (n[0] / 1e6, n[1] / 1e6, ''.join('GRYBO'[i] if n[2] >> i & 1 else '.' for i in range(5)),
                  ' open' if n[2] & 0x20 else '', ' '.join(f for b, f in ((1, 'hopo'), (2, 'tap'), (4, 'sp'), (8, 'SP-END')) if n[3] & b)))
