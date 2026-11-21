#!/usr/bin/env python3
"""The songs GH Vita comes with, so nobody starts with an empty library: original instrumental rock songs composed and
played right here (no samples, no recordings, nothing anyone else wrote), each a Clone Hero folder - song.ini,
notes.chart, guitar.ogg (the guitars), song.ogg (the rest), album.png - with guitar, rhythm, bass and drums charted on
all four difficulties from the very notes that are heard, so every note lines up with the music.

  mkstarter.py OUT_DIR [key ...]        (keys: see SONGS; none: all of them)
  mkstarter.py --pack assets/starter    (what the game ships: folders by key and list.txt)

Needs numpy, scipy, Pillow and ffmpeg. The same run gives the same songs (each has its own seed)."""
import argparse, math, os, random, subprocess, tempfile, wave
import numpy as np
from scipy import ndimage, signal

RATE = 44100
RES = 192                       # chart ticks a beat

MINOR = [0, 2, 3, 5, 7, 8, 10]
MAJOR = [0, 2, 4, 5, 7, 9, 11]

# each song: its style picks the riffs, beats and bass lines; its form is (section, bars, kind)
SONGS = [
    dict(key='ignition', name='Highway Ignition', artist='The Static Hearts', genre='Hard Rock', bpm=138, tonic=45,
         mode='minor', style='hard', seed=11, tiers=dict(guitar=3, rhythm=2, bass=2, drums=3),
         form=[('Intro', 4, 'intro'), ('Verse 1', 8, 'verse'), ('Chorus 1', 8, 'chorus'), ('Verse 2', 8, 'verse2'),
               ('Chorus 2', 8, 'chorus'), ('Bridge', 4, 'bridge'), ('Guitar Solo', 8, 'solo'),
               ('Chorus 3', 8, 'chorus'), ('Outro', 4, 'outro')],
         colors=((210, 70, 30), (30, 10, 10), (255, 200, 60))),
    dict(key='paper', name='Paper Kings', artist='Velvet Dynamo', genre='Pop Rock', bpm=124, tonic=50, mode='major',
         style='pop', seed=23, tiers=dict(guitar=1, rhythm=1, bass=1, drums=1),
         form=[('Intro', 4, 'intro'), ('Verse 1', 8, 'verse'), ('Chorus 1', 8, 'chorus'), ('Verse 2', 8, 'verse2'),
               ('Chorus 2', 8, 'chorus'), ('Bridge', 8, 'bridge'), ('Guitar Solo', 8, 'solo'),
               ('Chorus 3', 8, 'chorus'), ('Outro', 4, 'outro')],
         colors=((70, 150, 230), (240, 230, 210), (230, 60, 110))),
    dict(key='glass', name='Glass Thunder', artist='Iron Lanterns', genre='Metal', bpm=168, tonic=40, mode='minor',
         style='metal', seed=37, tiers=dict(guitar=5, rhythm=4, bass=4, drums=5),
         form=[('Intro', 8, 'intro'), ('Verse 1', 8, 'verse'), ('Chorus 1', 8, 'chorus'), ('Verse 2', 8, 'verse2'),
               ('Chorus 2', 8, 'chorus'), ('Breakdown', 8, 'bridge'), ('Guitar Solo', 16, 'solo'),
               ('Chorus 3', 8, 'chorus'), ('Outro', 4, 'outro')],
         colors=((120, 130, 150), (8, 10, 18), (90, 200, 255))),
    dict(key='lastlight', name='Last Light Run', artist='Neon Vultures', genre='Punk', bpm=184, tonic=43, mode='major',
         style='punk', seed=41, tiers=dict(guitar=2, rhythm=2, bass=2, drums=3),
         form=[('Intro', 4, 'intro'), ('Verse 1', 16, 'verse'), ('Chorus 1', 8, 'chorus'), ('Verse 2', 16, 'verse2'),
               ('Chorus 2', 8, 'chorus'), ('Guitar Solo', 8, 'solo'), ('Bridge', 8, 'bridge'),
               ('Chorus 3', 16, 'chorus'), ('Outro', 4, 'outro')],
         colors=((255, 60, 200), (20, 10, 40), (120, 255, 120))),
    dict(key='overdrive', name='Midnight Overdrive', artist='Cobalt Ruin', genre='Rock', bpm=150, tonic=42,
         mode='minor', style='drive', seed=53, tiers=dict(guitar=4, rhythm=3, bass=3, drums=3),
         form=[('Intro', 8, 'intro'), ('Verse 1', 8, 'verse'), ('Chorus 1', 8, 'chorus'), ('Verse 2', 8, 'verse2'),
               ('Chorus 2', 8, 'chorus'), ('Guitar Solo', 16, 'solo'), ('Bridge', 4, 'bridge'),
               ('Chorus 3', 8, 'chorus'), ('Outro', 4, 'outro')],
         colors=((40, 90, 220), (5, 8, 30), (255, 140, 40))),
]

PROGRESSIONS = {
    'minor': dict(intro=[[0, 5, 6, 0], [0, 0, 5, 6]], verse=[[0, 0, 5, 6], [0, 5, 2, 6], [0, 3, 5, 6]],
                  chorus=[[5, 6, 0, 0], [5, 2, 6, 0], [3, 5, 6, 0], [5, 6, 2, 6]],
                  bridge=[[3, 4, 5, 6], [5, 3, 0, 6], [3, 3, 5, 6]], solo=[[0, 5, 6, 0], [0, 3, 6, 5]]),
    'major': dict(intro=[[0, 4, 5, 3], [0, 3, 4, 4]], verse=[[0, 4, 5, 3], [0, 3, 4, 3], [0, 5, 3, 4]],
                  chorus=[[3, 0, 4, 5], [0, 4, 5, 3], [3, 4, 0, 5]], bridge=[[5, 3, 0, 4], [3, 4, 5, 5]],
                  solo=[[0, 4, 5, 3], [5, 3, 0, 4]]),
}

# the rhythm guitar: (beat, beats long, o open chord / m palm-muted root) a bar, by style and the section's kind
O, M = 'o', 'm'
RIFFS = {
    'hard': dict(verse=[(0, .5, O), (.5, .5, M), (1, .5, M), (1.5, .5, O), (2, .5, M), (2.5, .5, M), (3, .5, M),
                        (3.5, .5, O)],
                 chorus=[(0, 1.5, O), (1.5, 1, O), (2.5, 1.5, O)], bridge=[(0, 2, O), (2, 2, O)]),
    'pop': dict(verse=[(0, 1, M), (1, .5, M), (1.5, .5, M), (2, 1, M), (3, .5, M), (3.5, .5, O)],
                chorus=[(0, 1.5, O), (1.5, 1, O), (2.5, 1.5, O)], bridge=[(0, 4, O)]),
    'metal': dict(verse=[(b + d, l, O if b == 0 and d == 0 else M) for b in range(4)
                         for d, l in ((0, .5), (.5, .25), (.75, .25))],
                  chorus=[(0, 1, O), (1, .5, M), (1.5, .5, M), (2, 1, O), (3, .5, M), (3.5, .5, M)],
                  bridge=[(0, 2, O), (2, .5, M), (2.5, .5, M), (3, 1, O)]),
    'punk': dict(verse=[(i * .5, .5, O if i in (0, 3, 4) else M) for i in range(8)],
                 chorus=[(i * .5, .5, O) for i in range(8)], bridge=[(0, 1.5, O), (1.5, 1, O), (2.5, 1.5, O)]),
    'drive': dict(verse=[(i * .5, .5, O if i in (0, 3) else M) for i in range(8)],
                  chorus=[(0, .5, O), (.5, .5, O), (1, .5, M), (1.5, 1, O), (2.5, .5, M), (3, 1, O)],
                  bridge=[(0, 2, O), (2, 1, O), (3, 1, O)]),
}

# lead rhythms: two bars at a time, (beat, beats long)
LEAD_RHYTHMS = [
    [(0, 1), (1, .5), (1.5, .5), (2, 1), (3, 1), (4, 1.5), (5.5, .5), (6, 2)],
    [(0, .5), (.5, .5), (1, 1), (2, .5), (2.5, 1.5), (4, .5), (4.5, .5), (5, .5), (5.5, .5), (6, 2)],
    [(0, 1.5), (1.5, .5), (2, .5), (2.5, .5), (3, 1), (4, 3), (7, 1)],
    [(.5, .5), (1, .5), (1.5, .5), (2, 1), (3, .5), (3.5, .5), (4, 2), (6, .5), (6.5, .5), (7, 1)],
    [(0, 2), (2, 1), (3, 1), (4, 3), (7, 1)],
]


def beat_s(song):
    return 60.0 / song['bpm']


# ------------------------------------------------------------------------------------------------ composing

class Score:
    """what's played: each instrument's notes in beats, and where the sections are"""

    def __init__(self):
        self.rhythm = []        # (beat, beats, kind o/m, root pitch)
        self.lead = []          # (beat, beats, pitch, bend)
        self.bass = []          # (beat, beats, pitch)
        self.drums = []         # (beat, piece, velocity)
        self.pad = []           # (beat, beats, [pitches])
        self.sections = []      # (beat, name, kind, bars)
        self.chords = []        # per bar: root pitch class offset (scale degree's semitones from the tonic)
        self.beats = 0


def compose(song):
    rnd = random.Random(song['seed'])
    scale = MINOR if song['mode'] == 'minor' else MAJOR
    tonic, style = song['tonic'], song['style']
    sc = Score()
    prog = PROGRESSIONS[song['mode']]
    picked = {}                                         # one progression per kind of section, kept all song

    def progression(kind):
        k = {'verse2': 'verse', 'outro': 'intro'}.get(kind, kind)
        if k not in picked:
            picked[k] = rnd.choice(prog[k])
        return picked[k]

    def guitar_root(degree):
        p = tonic + scale[degree % 7]
        while p > 52:
            p -= 12
        while p < 40:
            p += 12
        return p

    lead_rhythm = {}

    def rhythm_for(kind):
        if kind not in lead_rhythm:
            pool = LEAD_RHYTHMS[:4] if style != 'pop' else [LEAD_RHYTHMS[4], LEAD_RHYTHMS[0], LEAD_RHYTHMS[2]]
            lead_rhythm[kind] = rnd.choice(pool)
        return lead_rhythm[kind]

    # the scale's notes where the lead plays
    lead_lo, lead_hi = 57, 81
    lead_scale = [p for p in range(lead_lo, lead_hi + 1) if (p - tonic) % 12 in scale]
    pent = [0, 3, 5, 7, 10] if song['mode'] == 'minor' else [0, 2, 4, 7, 9]
    pent_notes = [p for p in range(60, 86) if (p - tonic) % 12 in pent]

    def nearest(p, choices):
        return min(choices, key=lambda c: (abs(c - p), c))

    def chord_tones(degree):
        return [(tonic + scale[(degree + k) % 7]) % 12 for k in (0, 2, 4)]

    def melody(start_beat, bars, degrees, kind, contour_up=True, motif=None):
        """a phrase of `bars` over the chords; motif: a phrase to answer (its rhythm and shape, varied)"""
        out = []
        rhy = rhythm_for(kind)
        prev = nearest(lead_lo + 7 if kind != 'chorus' else lead_lo + 12, lead_scale)
        for two in range(0, bars, 2):
            base = start_beat + two * 4
            for i, (b, l) in enumerate(rhy):
                bar = two + int(b // 4)
                if bar >= bars:
                    break
                deg = degrees[bar % len(degrees)]
                tones = chord_tones(deg)
                strong = b % 2 == 0
                idx = lead_scale.index(prev)
                if motif and i < len(motif) and rnd.random() < .7:
                    want = motif[i] + (2 if two % 4 == 2 else 0)
                    p = nearest(want, lead_scale)
                elif strong:
                    p = nearest(prev + (2 if contour_up else -2), [q for q in lead_scale if q % 12 in tones])
                else:
                    r = rnd.random()
                    step = 1 if r < .6 else 2 if r < .85 else 0
                    up = (rnd.random() < .6) == contour_up
                    j = max(0, min(len(lead_scale) - 1, idx + (step if up else -step)))
                    p = lead_scale[j]
                # the last note of a phrase rests on a chord tone, held
                if i == len(rhy) - 1:
                    p = nearest(p, [q for q in lead_scale if q % 12 in tones])
                bend = l >= 1.5 and rnd.random() < .5
                out.append((base + b, l, p, bend))
                prev = p
        return out

    def solo(start_beat, bars, degrees):
        out = []
        fast = 0.25 if style in ('hard', 'metal', 'drive') else 0.5
        # call and answer, high
        out += melody(start_beat, 2, degrees, 'solo', True)
        first = [n[2] for n in out]
        out += melody(start_beat + 8, 2, degrees[2:] + degrees[:2], 'solo', False, motif=first)
        b = start_beat + 16
        for rep in range(max(1, bars // 8)):
            # runs: up and down the pentatonic in fours
            n = int(8 / fast)
            j = rnd.randrange(0, 4)
            direction = 1
            for k in range(n):
                p = pent_notes[max(0, min(len(pent_notes) - 1, j))]
                out.append((b + k * fast, fast, p, False))
                if k % 4 == 3:
                    j -= direction * 2        # a step back each four: the run rolls
                j += direction
                if j >= len(pent_notes) - 1 or (k == n // 2):
                    direction = -direction
            b += 8
            # the high one, bent up into and held
            top = pent_notes[-3]
            out += [(b, 1.5, top, True), (b + 1.5, .5, pent_notes[-4], False), (b + 2, 2, top, True)]
            b += 4
            # down to where it began
            j = len(pent_notes) - 5
            for k in range(6):
                out.append((b + k * .5, .5, pent_notes[max(0, j - k)], False))
            out.append((b + 3, 1, nearest(tonic + 24, lead_scale), False))
            b += 4
            if rep + 1 < max(1, bars // 8):
                out += melody(b, 4, degrees, 'solo', True, motif=first)
                b += 16
        return [n for n in out if n[0] < start_beat + bars * 4]

    beat = 4                                            # bar 0: the count-in
    hook = None
    for si, (name, bars, kind) in enumerate(song['form']):
        sc.sections.append((beat, name, kind, bars))
        degrees = progression(kind)
        riffkind = {'intro': 'chorus', 'outro': 'chorus', 'solo': 'verse', 'verse2': 'verse'}.get(kind, kind)
        riff = RIFFS[style][riffkind]
        last_section = si == len(song['form']) - 1
        for bar in range(bars):
            deg = degrees[bar % len(degrees)]
            root = guitar_root(deg)
            sc.chords.append(deg)
            b0 = beat + bar * 4
            if last_section and bar == bars - 1:
                # the end: one big chord, held, and the bass under it
                sc.rhythm.append((b0, 6, O, guitar_root(0)))
                sc.bass.append((b0, 6, guitar_root(0) - 12))
                continue
            for b, l, k in riff:
                sc.rhythm.append((b0 + b, l, k, root))
            # bass: by style
            broot = root - 12
            nxt = guitar_root(degrees[(bar + 1) % len(degrees)]) - 12
            if style == 'pop':
                pat = [(0, 1, broot), (1, .5, broot), (1.5, .5, broot + 12), (2, 1, broot + 7), (3, .5, broot),
                       (3.5, .5, broot + (2 if nxt > broot else -1))]
            elif style == 'metal' and riffkind == 'verse':
                pat = [(b, l, broot) for b, l, k in riff]
            elif kind == 'bridge' and style in ('hard', 'drive', 'metal'):
                pat = [(0, 2, broot), (2, 1, broot), (3, 1, broot + 7)]
            else:
                pat = [(i * .5, .5, broot) for i in range(8)]
                if bar % 4 == 3:            # walking into the next chord
                    step = 1 if nxt > broot else -1
                    pat[6] = (3, .5, broot + 2 * step if abs(nxt - broot) > 2 else broot + 7)
                    pat[7] = (3.5, .5, nxt - step)
            for b, l, p in pat:
                while p < 28:
                    p += 12
                sc.bass.append((b0 + b, l, p))
            if kind in ('chorus', 'bridge') and style != 'punk':
                tones = sorted({tonic + 12 + (t - tonic) % 12 for t in chord_tones(deg)})
                sc.pad.append((b0, 4, [t + 12 if t < 55 else t for t in tones]))
        # the lead
        if kind in ('intro', 'outro', 'chorus'):
            if hook is None or kind == 'chorus':
                ph = melody(beat, min(bars, 4), degrees, kind, True, motif=hook)
                if hook is None:
                    hook = [n[2] for n in ph]
            else:
                ph = melody(beat, min(bars, 4), degrees, kind, True, motif=hook)
            sc.lead += ph
            if bars > 4:
                # the second half answers the first (and comes down at its end)
                sc.lead += melody(beat + 16, bars - 4, degrees, kind, False, motif=[n[2] for n in ph])
            if last_section:
                sc.lead = [n for n in sc.lead if n[0] < beat + (bars - 1) * 4]
        elif kind == 'verse2':
            # the riff alone for the verse's first half, a melody over it in the second
            sc.lead += melody(beat + bars * 2, bars // 2, degrees, 'verse', True)
        elif kind == 'solo':
            sc.lead += solo(beat, bars, degrees)
        # the drums
        drums(sc, rnd, style, kind, beat, bars, si == 0, last_section)
        beat += bars * 4
    sc.beats = beat + 8
    # the count-in: four on the hi-hat
    for b in range(4):
        sc.drums.append((b, 'H', .8 if b else 1))
    sc.lead.sort()
    sc.rhythm.sort(key=lambda n: n[0])
    sc.drums.sort(key=lambda n: n[0])
    return sc


def drums(sc, rnd, style, kind, start, bars, first, last):
    """the beat for a section, a crash at its top and a fill at its end"""
    for bar in range(bars):
        b0 = start + bar * 4
        hits = []
        chorus = kind in ('chorus', 'outro', 'intro')
        if kind == 'bridge':
            hits += [(0, 'K'), (2, 'S'), (2.5, 'K')] + [(b, 'H') for b in range(4)]
            if style == 'metal':
                hits = [(0, 'K'), (.5, 'K'), (1, 'K'), (2, 'S'), (2.5, 'K'), (3, 'K'), (3.5, 'K')] + \
                       [(b, 'R') for b in range(4)]
        elif style == 'metal':
            if chorus or kind == 'solo':
                hits += [(i * .5, 'K') for i in range(8)] + [(1, 'S'), (3, 'S')] + [(b, 'C' if b % 2 == 0 else 'R')
                                                                                     for b in range(4)]
            else:
                hits += [(i * .25, 'K') for i in range(16) if i % 4 != 2 or rnd.random() < .5] + \
                        [(1, 'S'), (3, 'S')] + [(i * .5, 'H') for i in range(8)]
        elif style == 'punk':
            if chorus:
                hits += [(b, 'K') for b in range(4)] + [(b + .5, 'S') for b in range(4)] + \
                        [(b, 'R') for b in range(4)]
            else:
                hits += [(0, 'K'), (1.5, 'K'), (2, 'K'), (1, 'S'), (3, 'S')] + [(i * .5, 'H') for i in range(8)]
        else:
            kick = {'hard': [0, 2, 2.5], 'pop': [0, 1.5, 2.5], 'drive': [0, .5, 2, 2.5]}[style]
            if chorus:
                kick = kick + [1.5] if 1.5 not in kick else kick + [3.5]
            hits += [(k, 'K') for k in kick] + [(1, 'S'), (3, 'S')]
            if chorus or kind == 'solo':
                hits += [(i * .5, 'R' if style != 'pop' else 'HO' if i % 2 else 'H') for i in range(8)]
            else:
                hits += [(i * .5, 'H') for i in range(8)]
        if bar % 2 == 0 and (chorus or kind == 'solo') or bar == 0:
            hits = [h for h in hits if not (h[0] == 0 and h[1] in ('H', 'R', 'HO'))] + [(0, 'C')]
            if not any(h == (0, 'K') for h in hits):
                hits.append((0, 'K'))
        if bar == bars - 1 and not last:
            # the fill: the bar's second half on snare and toms
            fill = rnd.choice([
                [(2, 'S'), (2.25, 'S'), (2.5, 'T1'), (2.75, 'T1'), (3, 'T2'), (3.25, 'T2'), (3.5, 'T3'), (3.75, 'T3')],
                [(2, 'S'), (2.5, 'S'), (3, 'T1'), (3.5, 'T3')],
                [(3, 'S'), (3.25, 'S'), (3.5, 'S'), (3.75, 'S')],
                [(2, 'T1'), (2.5, 'T2'), (3, 'S'), (3.25, 'S'), (3.5, 'T3'), (3.75, 'T3')],
            ])
            start_fill = fill[0][0]
            hits = [h for h in hits if h[0] < start_fill or h[1] == 'K' and h[0] == start_fill] + fill
        if last and bar == bars - 1:
            hits = [(0, 'K'), (0, 'C'), (0, 'C2')]
        for b, piece in hits:
            vel = 1.0 if b == int(b) else .78
            if piece in ('H', 'R') and b % 1:
                vel = .7
            sc.drums.append((b0 + b, piece, vel))


# ------------------------------------------------------------------------------------------------ the instruments

def hz(p):
    return 440.0 * 2 ** ((p - 69) / 12)


def butter(kind, f, order=2):
    return signal.butter(order, f, btype=kind, fs=RATE, output='sos')


def filt(x, kind, f, order=2):
    return signal.sosfilt(butter(kind, f, order), x)


_pluck = {}


def pluck(pitch, dur, bright, decay, seed, bend=False, vibrato=0.0):
    """a plucked string (Karplus-Strong): a burst of noise going round a delay one period long, softened each time.
    bend: it starts two semitones under and is pushed up; vibrato: how wide it shakes (semitones) once held"""
    key = (pitch, round(dur, 3), bright, decay, seed, bend, vibrato)
    if key in _pluck:
        return _pluck[key]
    f = hz(pitch)
    period = RATE / f
    N = max(2, int(period - .5))
    rendered_f = RATE / (N + .5)
    n = int(dur * RATE)
    t = np.arange(n) / RATE
    # how fast the string's read: its pitch over time
    semis = np.zeros(n)
    if bend:
        semis += -2 * np.clip(1 - t / .12, 0, 1) ** 2
    if vibrato:
        depth = vibrato * np.clip((t - .18) / .3, 0, 1)
        semis += depth * np.sin(2 * np.pi * 5.6 * t)
    rate = (f / rendered_f) * 2 ** (semis / 12)
    pos = np.concatenate(([0.0], np.cumsum(rate)[:-1]))
    L = int(pos[-1]) + N + 4 if n else N + 4
    rng = np.random.default_rng(seed)
    exc = rng.uniform(-1, 1, N)
    exc = signal.lfilter([bright], [1, -(1 - bright)], exc)
    exc -= exc.mean()
    y = np.zeros(L)
    y[:N] = exc
    k = 1
    while k * N < L:
        a, b = k * N, min(L, (k + 1) * N)
        prev = y[a - N:b - N]
        prev1 = np.concatenate(([y[a - N - 1] if a - N - 1 >= 0 else 0.0], y[a - N:b - N - 1]))
        y[a:b] = decay * .5 * (prev + prev1)
        k += 1
    out = np.interp(pos, np.arange(L), y) if n else np.zeros(0)
    _pluck[key] = out
    return out


def place(buf, at_s, x, gain=1.0):
    s = int(round(at_s * RATE))
    if s >= len(buf) or s + len(x) <= 0:
        return
    if s < 0:
        x, s = x[-s:], 0
    e = min(len(buf), s + len(x))
    buf[s:e] += x[:e - s] * gain


def release(x, fade_s=.012):
    n = min(len(x), int(fade_s * RATE))
    if n:
        x = x.copy()
        x[-n:] *= np.linspace(1, 0, n)
    return x


def amp(x, gain, tone, tight=110, scoop=True):
    """a distorted amp and its speaker: the lows tightened before it, clipped, then the cabinet's roll-off"""
    x = filt(x, 'highpass', tight)
    x = np.tanh(x * gain) / np.tanh(gain)
    x = filt(x, 'highpass', 75)
    if scoop:
        b, a = signal.iirpeak(500, 1.2, fs=RATE)
        x = x - .35 * signal.lfilter(b, a, x)
    x = filt(x, 'lowpass', tone, 4)
    return x


def guitar_track(song, sc, seed, swing_ms):
    """the rhythm guitar once (twice played, one each side)"""
    bs = beat_s(song)
    rnd = random.Random(seed)
    buf = np.zeros(int((sc.beats * bs + 4) * RATE))
    notes = sc.rhythm
    for i, (b, l, kind, root) in enumerate(notes):
        nxt = notes[i + 1][0] if i + 1 < len(notes) else b + l + 2
        ring = min(nxt, b + l + (.5 if kind == O else 0)) - b
        dur = max(.05, ring * bs)
        at = b * bs + rnd.uniform(-swing_ms, swing_ms) / 1000
        if kind == M:
            x = pluck(root, min(dur, .22), .35, .985, seed * 7 + root) + .6 * pluck(root + 7, min(dur, .22), .35, .985,
                                                                                  seed * 7 + root + 1)
            x = x * np.exp(-np.arange(len(x)) / (RATE * .07))
            place(buf, at, release(x, .008), .9)
        else:
            for k, iv in enumerate((0, 7, 12)):
                x = pluck(root + iv, dur, .75, .998, seed * 13 + root * 3 + k)
                place(buf, at + k * .005, release(x), 1.0 if iv == 0 else .8)
    return amp(buf, 14, 4200, tight=85)


def lead_track(song, sc):
    bs = beat_s(song)
    buf = np.zeros(int((sc.beats * bs + 4) * RATE))
    for i, (b, l, p, bend) in enumerate(sc.lead):
        nxt = sc.lead[i + 1][0] if i + 1 < len(sc.lead) else b + l
        dur = max(.06, (min(nxt, b + l) - b) * bs + .02)
        x = pluck(p, dur, .85, .9996, 1000 + p, bend=bend, vibrato=.35 if dur > .45 else 0)
        place(buf, b * bs, release(x, .02), .9)
    return amp(buf, 22, 5200, tight=220, scoop=False)


def bass_track(song, sc):
    bs = beat_s(song)
    buf = np.zeros(int((sc.beats * bs + 4) * RATE))
    for i, (b, l, p) in enumerate(sc.bass):
        dur = max(.05, l * bs - .01)
        x = pluck(p, dur, .62, .9995, 500 + p)
        t = np.arange(len(x)) / RATE
        x = x + .15 * np.sin(2 * np.pi * hz(p) * t) * np.exp(-t / 1.2)
        place(buf, b * bs, release(x, .015))
    buf = np.tanh(buf * 2.6) / np.tanh(2.6)
    return filt(filt(buf, 'lowpass', 2400, 4), 'highpass', 35)


def pad_track(song, sc):
    bs = beat_s(song)
    buf = np.zeros(int((sc.beats * bs + 4) * RATE))
    for b, l, ps in sc.pad:
        n = int((l * bs + .4) * RATE)
        t = np.arange(n) / RATE
        env = np.minimum(1, t / .25) * np.minimum(1, np.maximum(0, (l * bs + .4 - t) / .4))
        x = np.zeros(n)
        for p in ps:
            for det in (-.08, .08):
                f = hz(p + det)
                for h in range(1, 7):
                    x += np.sin(2 * np.pi * f * h * t) / h
        place(buf, b * bs, x * env / len(ps) * .25)
    return filt(buf, 'lowpass', 1600, 2)


_hits = {}


def drum_hit(piece, seed):
    key = (piece, seed)
    if key in _hits:
        return _hits[key]
    rng = np.random.default_rng(seed)

    def noise(sec):
        return rng.uniform(-1, 1, int(sec * RATE))

    def t_of(x):
        return np.arange(len(x)) / RATE

    if piece == 'K':
        n = int(.35 * RATE)
        t = np.arange(n) / RATE
        f = 54 + 90 * np.exp(-t / .03)
        x = np.sin(2 * np.pi * np.cumsum(f) / RATE) * np.exp(-t / .16)
        click = filt(noise(.006), 'highpass', 2500) * np.linspace(1, 0, int(.006 * RATE))
        x[:len(click)] += click * .5
        x = np.tanh(x * 1.6)
    elif piece == 'S':
        t = np.arange(int(.25 * RATE)) / RATE
        tone = (np.sin(2 * np.pi * 185 * t) + .5 * np.sin(2 * np.pi * 330 * t)) * np.exp(-t / .05)
        nz = filt(noise(.25), 'bandpass', [1200, 9000]) * np.exp(-t / .07)
        x = .55 * tone + 1.1 * nz
    elif piece in ('H', 'HO'):
        sec = .06 if piece == 'H' else .32
        x = metal(rng, sec + .02, 7000) * np.exp(-t_of(np.zeros(int((sec + .02) * RATE))) / (sec / 3))
        x *= .5
    elif piece == 'R':
        x = metal(rng, .9, 4000)
        t = t_of(x)
        x = x * (.35 * np.exp(-t / .5) + .4 * np.exp(-t / .03)) * .6
    elif piece in ('C', 'C2'):
        x = metal(rng, 2.2, 3000) + .6 * filt(noise(2.2), 'highpass', 5000)
        t = t_of(x)
        x = x * np.exp(-t / .7) * .55
    else:                                               # the toms: high, middle, floor
        f0 = {'T1': 210, 'T2': 150, 'T3': 105}[piece]
        n = int(.45 * RATE)
        t = np.arange(n) / RATE
        f = f0 * (1 + .5 * np.exp(-t / .03))
        x = np.sin(2 * np.pi * np.cumsum(f) / RATE) * np.exp(-t / .2)
        x += .2 * filt(rng.uniform(-1, 1, n), 'lowpass', 3000) * np.exp(-t / .03)
    _hits[key] = x.astype(np.float64)
    return _hits[key]


def metal(rng, sec, hp):
    """a cymbal's ring: square waves at inharmonic pitches, mixed and kept high"""
    n = int(sec * RATE)
    t = np.arange(n) / RATE
    x = np.zeros(n)
    for f in (205.3, 304.4, 369.6, 522.7, 540.0, 800.0):
        f *= rng.uniform(.97, 1.03)
        x += np.sign(np.sin(2 * np.pi * f * t + rng.uniform(0, 6.28)))
    x = filt(x, 'highpass', hp, 4)
    return x / 6


PAN = {'K': 0, 'S': 0, 'H': .35, 'HO': .35, 'R': -.4, 'C': -.45, 'C2': .5, 'T1': .3, 'T2': -.1, 'T3': -.4}
LEVEL = {'K': 1.0, 'S': .8, 'H': .32, 'HO': .3, 'R': .28, 'C': .45, 'C2': .4, 'T1': .7, 'T2': .7, 'T3': .75}


def drum_tracks(song, sc):
    """the kit in stereo, and how much of the snare and toms goes to the room"""
    bs = beat_s(song)
    n = int((sc.beats * bs + 4) * RATE)
    L, R, room = np.zeros(n), np.zeros(n), np.zeros(n)
    for i, (b, piece, vel) in enumerate(sc.drums):
        x = drum_hit(piece, i % 4)
        g = LEVEL[piece] * vel
        pan = PAN[piece]
        place(L, b * bs, x, g * (1 - max(0, pan)))
        place(R, b * bs, x, g * (1 + min(0, pan)))
        if piece in ('S', 'T1', 'T2', 'T3'):
            place(room, b * bs, x, g * .5)
    return L, R, room


def reverb(x, seconds=1.4, seed=5):
    rng = np.random.default_rng(seed)
    n = int(seconds * RATE)
    t = np.arange(n) / RATE
    ir = rng.uniform(-1, 1, n) * np.exp(-t / (seconds / 5))
    ir = filt(ir, 'lowpass', 6000)
    ir[:int(.012 * RATE)] = 0                          # a little gap before the room answers
    ir /= np.sqrt(np.sum(ir ** 2))
    return signal.fftconvolve(x, ir)[:len(x)] * .5


# ------------------------------------------------------------------------------------------------ the chart

def fret_map(pitches):
    """the lead's notes in a stretch, onto five frets: higher notes further up the neck, each change of note a change of
    fret where it can be"""
    distinct = sorted(set(pitches))
    if len(distinct) <= 5:
        spread = {p: i for i, p in enumerate(distinct)}
        off = max(0, min(5 - len(distinct), round((np.mean(distinct) - 57) / 24 * (5 - len(distinct)))))
        frets = [spread[p] + off for p in pitches]
    else:
        lo, hi = distinct[0], distinct[-1]
        frets = [round((p - lo) / (hi - lo) * 4) for p in pitches]
    for i in range(1, len(frets)):
        if pitches[i] != pitches[i - 1] and frets[i] == frets[i - 1]:
            up = pitches[i] > pitches[i - 1]
            frets[i] = min(4, frets[i] + 1) if up and frets[i] < 4 or frets[i] == 0 else frets[i] - 1
    return frets


def chart_lead(sc, bs_ticks, start, end):
    """the lead between two beats as (tick, frets, sustain), two bars at a time"""
    notes = [n for n in sc.lead if start <= n[0] < end]
    out = []
    w = 0
    while w < len(notes):
        win_end = notes[w][0] // 8 * 8 + 8
        j = w
        while j < len(notes) and notes[j][0] < win_end:
            j += 1
        group = notes[w:j]
        frets = fret_map([n[2] for n in group])
        for (b, l, p, bend), f in zip(group, frets):
            out.append((round(b * RES), [f], sustain(l)))
        w = j
    return out


def sustain(beats):
    return round((beats - .25) * RES) if beats >= 1 else 0


def chart_chords(notes, start, end, single_mutes=True):
    """rhythm guitar (or bass) roots as frets: each section's roots in order up the neck; an open power chord is two
    frets, a palm-muted one the lower of them"""
    sel = [n for n in notes if start <= n[0] < end]
    roots = sorted({n[3] for n in sel})
    pos = {r: i for i, r in enumerate(roots)}
    span = max(1, len(roots) - 1)
    out = []
    for b, l, kind, root in sel:
        f = round(pos[root] / span * 3) if len(roots) > 1 else 1
        frets = [f] if kind == M and single_mutes else [f, f + 1]
        out.append((round(b * RES), frets, sustain(l) if kind == O else 0))
    return out


def chart_bass(sc, start, end):
    sel = [n for n in sc.bass if start <= n[0] < end]
    out = []
    w = 0
    while w < len(sel):
        win_end = sel[w][0] // 16 * 16 + 16
        j = w
        while j < len(sel) and sel[j][0] < win_end:
            j += 1
        group = sel[w:j]
        frets = fret_map([n[2] + 24 for n in group])
        for (b, l, p), f in zip(group, frets):
            out.append((round(b * RES), [f], sustain(l)))
        w = j
    return out


def reduce(notes, diff, max_fret_count):
    """an easier chart from a harder: Hard drops what isn't on an eighth, Medium keeps a note an eighth or more from the
    last and three frets, Easy one a beat or more from the last, three frets, no chords"""
    if diff == 'Expert':
        return notes
    grid = {'Hard': RES // 2, 'Medium': RES // 2, 'Easy': RES}[diff]
    gap = {'Hard': RES // 4, 'Medium': RES // 2, 'Easy': RES}[diff]
    top = {'Hard': 3, 'Medium': 2, 'Easy': 2}[diff]
    out, last = [], -10 ** 9
    for t, frets, sus in notes:
        if t % grid or t - last < gap:
            continue
        fs = sorted({min(top, round(f * top / 4)) for f in frets})
        if diff == 'Easy' or (diff == 'Medium' and len(fs) > 1 and fs[-1] - fs[0] > 1):
            fs = fs[:1]
        if len(fs) > max_fret_count:
            fs = fs[:max_fret_count]
        out.append((t, fs, sus))
        last = t
    return out


DRUM_PAD = {'K': [0], 'S': [1], 'H': [2, 66], 'HO': [2, 66], 'R': [3, 67], 'C': [4, 68], 'C2': [4, 68],
            'T1': [2], 'T2': [3], 'T3': [4]}


def chart_drums(sc, diff):
    by_tick = {}
    for b, piece, vel in sc.drums:
        if b < 4:
            continue                                    # the count-in isn't played
        t = round(b * RES)
        if diff in ('Medium', 'Easy') and piece in ('H', 'HO', 'R') and t % (RES if diff == 'Medium' else 2 * RES):
            continue
        if diff == 'Hard' and piece in ('H', 'HO') and t % (RES // 2):
            continue
        if diff != 'Expert' and piece == 'K' and t % (RES // 2 if diff == 'Hard' else RES):
            continue
        if diff == 'Easy' and piece in ('T1', 'T2', 'T3') and t % (RES // 2):
            continue
        if diff == 'Easy' and piece in ('H', 'HO', 'R'):
            continue
        by_tick.setdefault(t, set()).update(DRUM_PAD[piece])
    out = []
    for t in sorted(by_tick):
        pads = by_tick[t]
        # a cymbal and the tom on the same pad: the cymbal; no more than two hands (and the kick)
        hands = sorted(p for p in pads if 1 <= p <= 4)
        if len(hands) > 2:
            keep = [1] if 1 in hands else []
            keep += [p for p in hands if p != 1][-(2 - len(keep)):]
            pads = {p for p in pads if p == 0 or p in keep or (p >= 66 and p - 64 in keep)}
        out.append((t, sorted(pads), 0))
    return out


def track_text(notes, sp, solos, drums=False):
    lines = []
    for t, frets, sus in notes:
        for f in frets:
            lines.append((t, 0, f'N {f} {0 if drums else sus}'))
    for t, l in sp:
        lines.append((t, 1, f'S 2 {l}'))
    for a, b in solos:
        lines += [(a, 2, 'E solo'), (b, 2, 'E soloend')]
    lines.sort(key=lambda x: (x[0], x[1]))
    return '\n'.join(f'  {t} = {s}' for t, _, s in lines)


def write_chart(song, sc, folder):
    bs = beat_s(song)
    # guitar: the lead where it plays, the rhythm guitar where it doesn't
    guitar = []
    for (beat, name, kind, bars) in sc.sections:
        end = beat + bars * 4
        lead_here = [n for n in sc.lead if beat <= n[0] < end]
        if lead_here and kind != 'verse2':
            guitar += chart_lead(sc, RES, beat, end)
        elif lead_here:
            # the second verse: the riff for its first half, the melody over it after
            mid = beat + bars * 2
            guitar += chart_chords(sc.rhythm, beat, mid) + chart_lead(sc, RES, mid, end)
        else:
            guitar += chart_chords(sc.rhythm, beat, end)
    rhythm = []
    for (beat, name, kind, bars) in sc.sections:
        rhythm += chart_chords(sc.rhythm, beat, beat + bars * 4)
    bass = []
    for (beat, name, kind, bars) in sc.sections:
        bass += chart_bass(sc, beat, beat + bars * 4)
    # Star Power: the choruses' first two bars, the solo's last two, the second verse's middle
    sp = []
    solos = []
    for (beat, name, kind, bars) in sc.sections:
        if kind == 'chorus':
            sp.append((round(beat * RES), 8 * RES - 1))
        elif kind == 'verse2':
            sp.append((round((beat + 16) * RES), 8 * RES - 1))
        elif kind == 'solo':
            sp.append((round((beat + (bars - 2) * 4) * RES), 8 * RES - 1))
            solos.append((round(beat * RES), round((beat + bars * 4) * RES) - 1))
    ev = '\n'.join(f'  {round(b * RES)} = E "section {n}"' for b, n, k, bars in sc.sections)
    ev += f'\n  {round((sc.beats - 4) * RES)} = E "end"'
    parts = [f'[Song]\n{{\n  Name = "{song["name"]}"\n  Artist = "{song["artist"]}"\n  Charter = "GH Vita"\n'
             f'  Album = "GH Vita Starter Pack"\n  Year = ", 2026"\n  Offset = 0\n  Resolution = {RES}\n'
             f'  Genre = "{song["genre"]}"\n  MediaType = "cd"\n}}',
             f'[SyncTrack]\n{{\n  0 = TS 4\n  0 = B {song["bpm"] * 1000}\n}}', f'[Events]\n{{\n{ev}\n}}']
    for d in ('Expert', 'Hard', 'Medium', 'Easy'):
        parts.append(f'[{d}Single]\n{{\n{track_text(reduce(guitar, d, 3), sp, solos)}\n}}')
        parts.append(f'[{d}DoubleRhythm]\n{{\n{track_text(reduce(rhythm, d, 3), sp, [])}\n}}')
        parts.append(f'[{d}DoubleBass]\n{{\n{track_text(reduce(bass, d, 1), sp, [])}\n}}')
        parts.append(f'[{d}Drums]\n{{\n{track_text(chart_drums(sc, d), sp, [], True)}\n}}')
    with open(os.path.join(folder, 'notes.chart'), 'w') as f:
        f.write('\n'.join(parts) + '\n')
    counts = {'guitar': len(guitar), 'rhythm': len(rhythm), 'bass': len(bass), 'drums': len(chart_drums(sc, 'Expert'))}
    return counts


# ------------------------------------------------------------------------------------------------ the cover

def write_cover(song, path):
    from PIL import Image, ImageDraw, ImageFilter, ImageFont
    rnd = random.Random(song['seed'] * 3)
    W = 512
    c1, c2, c3 = song['colors']
    img = Image.new('RGB', (W, W), c2)
    d = ImageDraw.Draw(img)
    # a glow behind, rings and rays out of it
    cx, cy = W * rnd.uniform(.35, .65), W * rnd.uniform(.3, .45)
    for r in range(300, 0, -6):
        k = r / 300
        col = tuple(int(c2[i] * k + c1[i] * (1 - k)) for i in range(3))
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=col)
    for i in range(14):
        a = rnd.uniform(0, 2 * math.pi)
        w = rnd.uniform(.02, .07)
        pts = [(cx, cy), (cx + 900 * math.cos(a - w), cy + 900 * math.sin(a - w)),
               (cx + 900 * math.cos(a + w), cy + 900 * math.sin(a + w))]
        d.polygon(pts, fill=tuple(min(255, int(v * 1.15)) for v in c1))
    for r in (70, 120, 180):
        d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=c3, width=3)
    img = img.filter(ImageFilter.GaussianBlur(1.2))
    d = ImageDraw.Draw(img)
    fonts = '/usr/share/fonts/truetype/dejavu/'
    big = ImageFont.truetype(fonts + 'DejaVuSans-Bold.ttf', 46)
    small = ImageFont.truetype(fonts + 'DejaVuSans-Bold.ttf', 30)

    def text(s, y, font, fill):
        size = font.size
        f = font
        while d.textlength(s, font=f) > W - 50 and size > 14:
            size -= 2
            f = ImageFont.truetype(f.path, size)
        x = (W - d.textlength(s, font=f)) / 2
        d.text((x + 3, y + 3), s, font=f, fill=(0, 0, 0))
        d.text((x, y), s, font=f, fill=fill)

    d.rectangle([0, W - 150, W, W], fill=tuple(int(v * .6) for v in c2))
    text(song['artist'].upper(), W - 138, small, c3)
    text(song['name'], W - 90, big, (255, 255, 255))
    img.save(path)


# ------------------------------------------------------------------------------------------------ putting it together

def write_ogg(stereo, path):
    pcm = (np.clip(stereo, -1, 1) * 32000).astype('<i2')
    with tempfile.TemporaryDirectory() as tmp:
        w = os.path.join(tmp, 'a.wav')
        with wave.open(w, 'wb') as f:
            f.setnchannels(2)
            f.setsampwidth(2)
            f.setframerate(RATE)
            f.writeframes(pcm.tobytes())
        subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-i', w, '-c:a', 'libvorbis', '-q:a', '3', path],
                       check=True)


def make(song, out, pack=False):
    sc = compose(song)
    # (packed for the game, each under its key: the Vita's packager can't take a file name with spaces)
    folder = os.path.join(out, song['key'] if pack else f'{song["artist"]} - {song["name"]}')
    os.makedirs(folder, exist_ok=True)
    counts = write_chart(song, sc, folder)
    # the band, each on its own and then together
    gl = guitar_track(song, sc, 1, 6)
    gr = guitar_track(song, sc, 2, 6)
    lead = lead_track(song, sc)
    bass = bass_track(song, sc)
    pad = pad_track(song, sc)
    dl, dr, room = drum_tracks(song, sc)
    n = min(len(gl), len(gr), len(lead), len(bass), len(pad), len(dl))

    def fit(x):
        return x[:n] / (np.max(np.abs(x[:n])) + 1e-9)

    gl, gr, lead, bass, pad = fit(gl), fit(gr), fit(lead), fit(bass), fit(pad)
    dpk = max(np.max(np.abs(dl[:n])), np.max(np.abs(dr[:n]))) + 1e-9
    dl, dr, room = dl[:n] / dpk, dr[:n] / dpk, room[:n] / dpk
    # levels, by how loud each sounds (RMS) rather than its peaks
    def rms(x):
        return np.sqrt(np.mean(x ** 2)) + 1e-9
    g_rhy = .19 / rms(gl)
    g_lead = .2 / rms(lead[np.abs(lead) > 1e-4]) if np.any(np.abs(lead) > 1e-4) else 0
    g_bass = .17 / rms(bass)
    g_drum = .22 / rms(np.concatenate([dl, dr]))
    g_pad = .05 / rms(pad[np.abs(pad) > 1e-4]) if np.any(np.abs(pad) > 1e-4) else 0
    lead_room = reverb(lead * g_lead, 1.6, 7)
    room_l = reverb(room * g_drum, 1.1, 9) + reverb(pad * g_pad, 2.0, 11)
    room_r = reverb(room * g_drum, 1.1, 10) + reverb(pad * g_pad, 2.0, 12)
    guitars = np.stack([gl * g_rhy * .95 + gr * g_rhy * .25 + lead * g_lead * .8 + lead_room * .35,
                        gr * g_rhy * .95 + gl * g_rhy * .25 + lead * g_lead * .9 + lead_room * .35], 1)
    rest = np.stack([dl * g_drum + bass * g_bass + pad * g_pad * .9 + room_l * .3,
                     dr * g_drum + bass * g_bass + pad * g_pad * 1.1 + room_r * .3], 1)
    rest = signal.sosfilt(butter('highpass', 38, 4), rest, axis=0)     # nothing under the kick's and bass's notes
    # the whole mix squeezed a little (the same gain on both halves, so together they're the mix), to -1 dB
    mix = guitars + rest
    env = np.sqrt(signal.sosfilt(butter('lowpass', 8), np.mean(mix ** 2, 1)).clip(1e-12))
    thr = .25
    gain = np.where(env > thr, (thr / env) ** (1 - 1 / 2.5), 1.0)[:, None]
    guitars, rest = guitars * gain, rest * gain
    # up to about the loudness of a record (-12 dB RMS), its peaks held under -1 dB by a limiter that sees them coming
    mix = guitars + rest
    lift = 10 ** (-12 / 20) / np.sqrt(np.mean(mix ** 2))
    peaks = np.max(np.abs(mix), 1) * lift
    want = np.minimum(1, .8 / np.maximum(peaks, 1e-9))
    look = int(.005 * RATE)
    want = ndimage.minimum_filter1d(want, 2 * look + 1)
    # let go slowly (a fast release would be heard as distortion), never above what the peaks allow
    smooth = signal.lfilter([.0015], [1, -.9985], want - 1) + 1
    lim = np.minimum(want, smooth)
    lim = np.minimum(lim, ndimage.minimum_filter1d(lim, look))
    guitars, rest = guitars * (lift * lim)[:, None], rest * (lift * lim)[:, None]
    peak = np.max(np.abs(guitars + rest))
    if peak > .8:           # (and room for the encoding's own overshoot)
        guitars, rest = guitars * (.8 / peak), rest * (.8 / peak)
    # the end: a fade over its last two seconds
    fade = min(len(guitars), int(2 * RATE))
    ramp = np.linspace(1, 0, fade)[:, None]
    guitars[-fade:] *= ramp
    rest[-fade:] *= ramp
    write_ogg(guitars, os.path.join(folder, 'guitar.ogg'))
    write_ogg(rest, os.path.join(folder, 'song.ogg'))
    write_cover(song, os.path.join(folder, 'album.png'))
    length = n / RATE
    chorus = next((b for b, nm, k, bars in sc.sections if k == 'chorus'), 16)
    t = song['tiers']
    with open(os.path.join(folder, 'song.ini'), 'w') as f:
        f.write(f'[song]\nname = {song["name"]}\nartist = {song["artist"]}\nalbum = GH Vita Starter Pack\n'
                f'genre = {song["genre"]}\nyear = 2026\ncharter = GH Vita\nsong_length = {int(length * 1000)}\n'
                f'preview_start_time = {int(chorus * beat_s(song) * 1000)}\ndelay = 0\n'
                f'diff_guitar = {t["guitar"]}\ndiff_rhythm = {t["rhythm"]}\ndiff_bass = {t["bass"]}\n'
                f'diff_drums = {t["drums"]}\ndiff_band = {round(sum(t.values()) / 4)}\n'
                f'loading_phrase = An original song made for GH Vita: play it, share it, keep it.\n')
    print(f'{folder}: {length:.0f} s, notes {counts}')
    return folder


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('keys', nargs='*')
    ap.add_argument('--pack', action='store_true',
                    help="for the game's assets/starter: folders by key, and list.txt (key, tab, the songs folder's name)")
    a = ap.parse_args()
    for s in SONGS:
        if not a.keys or s['key'] in a.keys:
            make(s, a.out, a.pack)
    if a.pack:
        with open(os.path.join(a.out, 'list.txt'), 'w') as f:
            for s in SONGS:
                if os.path.isdir(os.path.join(a.out, s['key'])):
                    f.write(f'{s["key"]}\t{s["artist"]} - {s["name"]}\n')
