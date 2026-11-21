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

# the songs' version: a device with an older copy of one (still there, not deleted) gets this one over it
STARTER_VERSION = 2

MINOR = [0, 2, 3, 5, 7, 8, 10]
MAJOR = [0, 2, 4, 5, 7, 9, 11]

# each song: its style picks the riffs, beats and bass lines; its form is (section, bars, kind)
SONGS = [
    dict(key='ignition', motif='road', name='Highway Ignition', artist='The Static Hearts', genre='Hard Rock', bpm=138, tonic=45,
         mode='minor', style='hard', seed=11, tiers=dict(guitar=3, rhythm=2, bass=2, drums=3),
         form=[('Intro', 4, 'intro'), ('Verse 1', 8, 'verse'), ('Chorus 1', 8, 'chorus'), ('Verse 2', 8, 'verse2'),
               ('Chorus 2', 8, 'chorus'), ('Bridge', 4, 'bridge'), ('Guitar Solo', 8, 'solo'),
               ('Chorus 3', 8, 'chorus'), ('Outro', 4, 'outro')],
         colors=((210, 70, 30), (30, 10, 10), (255, 200, 60))),
    dict(key='paper', motif='crowns', name='Paper Kings', artist='Velvet Dynamo', genre='Pop Rock', bpm=124, tonic=50, mode='major',
         style='pop', seed=23, tiers=dict(guitar=1, rhythm=1, bass=1, drums=1),
         form=[('Intro', 4, 'intro'), ('Verse 1', 8, 'verse'), ('Chorus 1', 8, 'chorus'), ('Verse 2', 8, 'verse2'),
               ('Chorus 2', 8, 'chorus'), ('Bridge', 8, 'bridge'), ('Guitar Solo', 8, 'solo'),
               ('Chorus 3', 8, 'chorus'), ('Outro', 4, 'outro')],
         colors=((70, 150, 230), (240, 230, 210), (230, 60, 110))),
    dict(key='glass', motif='shards', name='Glass Thunder', artist='Iron Lanterns', genre='Metal', bpm=168, tonic=40, mode='minor',
         style='metal', seed=37, tiers=dict(guitar=5, rhythm=4, bass=4, drums=5),
         form=[('Intro', 8, 'intro'), ('Verse 1', 8, 'verse'), ('Chorus 1', 8, 'chorus'), ('Verse 2', 8, 'verse2'),
               ('Chorus 2', 8, 'chorus'), ('Breakdown', 8, 'bridge'), ('Guitar Solo', 16, 'solo'),
               ('Chorus 3', 8, 'chorus'), ('Outro', 4, 'outro')],
         colors=((120, 130, 150), (8, 10, 18), (90, 200, 255))),
    dict(key='lastlight', motif='stencil', name='Last Light Run', artist='Neon Vultures', genre='Punk', bpm=184, tonic=43, mode='major',
         style='punk', seed=41, tiers=dict(guitar=2, rhythm=2, bass=2, drums=3),
         form=[('Intro', 4, 'intro'), ('Verse 1', 16, 'verse'), ('Chorus 1', 8, 'chorus'), ('Verse 2', 16, 'verse2'),
               ('Chorus 2', 8, 'chorus'), ('Guitar Solo', 8, 'solo'), ('Bridge', 8, 'bridge'),
               ('Chorus 3', 16, 'chorus'), ('Outro', 4, 'outro')],
         colors=((255, 60, 200), (20, 10, 40), (120, 255, 120))),
    dict(key='overdrive', motif='gauge', name='Midnight Overdrive', artist='Cobalt Ruin', genre='Rock', bpm=150, tonic=42,
         mode='minor', style='drive', seed=53, tiers=dict(guitar=4, rhythm=3, bass=3, drums=3),
         form=[('Intro', 8, 'intro'), ('Verse 1', 8, 'verse'), ('Chorus 1', 8, 'chorus'), ('Verse 2', 8, 'verse2'),
               ('Chorus 2', 8, 'chorus'), ('Guitar Solo', 16, 'solo'), ('Bridge', 4, 'bridge'),
               ('Chorus 3', 8, 'chorus'), ('Outro', 4, 'outro')],
         colors=((40, 90, 220), (5, 8, 30), (255, 140, 40))),
    dict(key='northern', motif='aurora', name='Northern Lights', artist='Saltwater Radio', genre='Power Ballad', bpm=76, tonic=43,
         mode='major', style='ballad', seed=61, tiers=dict(guitar=1, rhythm=1, bass=0, drums=1),
         form=[('Intro', 4, 'intro'), ('Verse 1', 8, 'verse'), ('Chorus 1', 8, 'chorus'), ('Verse 2', 8, 'verse2'),
               ('Chorus 2', 8, 'chorus'), ('Guitar Solo', 8, 'solo'), ('Chorus 3', 8, 'chorus'), ('Outro', 4, 'outro')],
         colors=((40, 120, 140), (8, 20, 40), (170, 255, 220))),
    dict(key='gravel', motif='poles', name='Gravel Road Shuffle', artist='The Dusty Rails', genre='Blues Rock', bpm=116, tonic=40,
         mode='major', style='shuffle', swing=True, blues=True, prog=[0, 0, 0, 0, 3, 3, 0, 0, 4, 3, 0, 4], seed=67,
         tiers=dict(guitar=2, rhythm=2, bass=2, drums=2),
         form=[('Intro', 4, 'intro'), ('Verse 1', 12, 'verse'), ('Chorus 1', 12, 'chorus'), ('Verse 2', 12, 'verse2'),
               ('Guitar Solo', 24, 'solo'), ('Chorus 2', 12, 'chorus'), ('Outro', 4, 'outro')],
         colors=((200, 140, 60), (40, 20, 10), (255, 230, 150))),
    dict(key='voltage', motif='disco', name='Voltage Parade', artist='Mister Lumen', genre='Funk Rock', bpm=106, tonic=40,
         mode='minor', style='funk', prog={'verse': [0, 0, 0, 3], 'chorus': [5, 6, 0, 0]}, seed=71,
         tiers=dict(guitar=3, rhythm=4, bass=4, drums=3),
         form=[('Intro', 4, 'intro'), ('Verse 1', 8, 'verse'), ('Chorus 1', 8, 'chorus'), ('Verse 2', 8, 'verse2'),
               ('Chorus 2', 8, 'chorus'), ('Bridge', 8, 'bridge'), ('Guitar Solo', 8, 'solo'), ('Chorus 3', 8, 'chorus'),
               ('Outro', 4, 'outro')],
         colors=((250, 190, 30), (60, 10, 80), (80, 240, 255))),
    dict(key='sawtooth', motif='saw', name='Sawtooth Saints', artist='Grave Circuit', genre='Thrash Metal', bpm=200, tonic=40,
         mode='minor', style='thrash', seed=79, tiers=dict(guitar=6, rhythm=5, bass=5, drums=6),
         form=[('Intro', 8, 'intro'), ('Verse 1', 16, 'verse'), ('Chorus 1', 8, 'chorus'), ('Verse 2', 16, 'verse2'),
               ('Chorus 2', 8, 'chorus'), ('Breakdown', 8, 'bridge'), ('Guitar Solo', 16, 'solo'),
               ('Chorus 3', 8, 'chorus'), ('Outro', 4, 'outro')],
         colors=((180, 20, 20), (10, 10, 10), (230, 230, 230))),
    dict(key='desert', motif='dunes', name='Desert Engine', artist='Mammoth Mile', genre='Stoner Rock', bpm=84, tonic=38, low=38,
         mode='minor', style='stoner', seed=83, tiers=dict(guitar=2, rhythm=2, bass=2, drums=2),
         form=[('Intro', 4, 'intro'), ('Verse 1', 8, 'verse'), ('Chorus 1', 8, 'chorus'), ('Verse 2', 8, 'verse2'),
               ('Chorus 2', 8, 'chorus'), ('Guitar Solo', 8, 'solo'), ('Chorus 3', 8, 'chorus'), ('Outro', 4, 'outro')],
         colors=((230, 120, 40), (50, 25, 10), (255, 210, 120))),
]

PROGRESSIONS = {
    'minor': dict(intro=[[0, 5, 6, 0], [0, 0, 5, 6]], verse=[[0, 0, 5, 6], [0, 5, 2, 6], [0, 3, 5, 6]],
                  chorus=[[5, 6, 0, 0], [5, 2, 6, 0], [3, 5, 6, 0], [5, 6, 2, 6]],
                  bridge=[[3, 4, 5, 6], [5, 3, 0, 6], [3, 3, 5, 6]], solo=[[0, 5, 6, 0], [0, 3, 6, 5]]),
    'major': dict(intro=[[0, 4, 5, 3], [0, 3, 4, 4]], verse=[[0, 4, 5, 3], [0, 3, 4, 3], [0, 5, 3, 4]],
                  chorus=[[3, 0, 4, 5], [0, 4, 5, 3], [3, 4, 0, 5]], bridge=[[5, 3, 0, 4], [3, 4, 5, 5]],
                  solo=[[0, 4, 5, 3], [5, 3, 0, 4]]),
}

# the rhythm guitar: (beat, beats long, kind, step) a bar, by style and the section's kind. Kinds: o an open power
# chord, m the root palm-muted, c a clean picked note (an arpeggio), b a boogie dyad (the root and a note `step`
# semitones over it), g a dead ghost note (heard, not charted). step: scale steps up from the chord's root (b: semitones)
O, M, C, B, G = 'o', 'm', 'c', 'b', 'g'
LINE_STYLES = ('ballad', 'shuffle', 'funk', 'thrash', 'stoner')      # their riffs are lines: the steps every bar
RIFFS = {
    'hard': dict(verse=[(0, .5, O), (.5, .5, M), (1, .5, M), (1.5, .5, O), (2, .5, M), (2.5, .5, M), (3, .5, M, 2),
                        (3.5, .5, M, 3)],
                 chorus=[(0, 1.5, O), (1.5, 1, O), (2.5, 1.5, O)], bridge=[(0, 2, O), (2, 2, O)]),
    'pop': dict(verse=[(0, 1, M), (1, .5, M), (1.5, .5, M), (2, 1, M), (3, .5, M, 2), (3.5, .5, O)],
                chorus=[(0, 1.5, O), (1.5, 1, O), (2.5, 1.5, O)], bridge=[(0, 4, O)]),
    'metal': dict(verse=[(b + d, l, O if b == 0 and d == 0 else M, 1 if b == 3 else 0) for b in range(4)
                         for d, l in ((0, .5), (.5, .25), (.75, .25))],
                  chorus=[(0, 1, O), (1, .5, M), (1.5, .5, M), (2, 1, O), (3, .5, M), (3.5, .5, M)],
                  bridge=[(0, 2, O), (2, .5, M), (2.5, .5, M), (3, 1, O)]),
    'punk': dict(verse=[(i * .5, .5, O if i in (0, 3, 4, 6, 7) else M, 3 if i >= 6 else 0) for i in range(8)],
                 chorus=[(i * .5, .5, O) for i in range(8)], bridge=[(0, 1.5, O), (1.5, 1, O), (2.5, 1.5, O)]),
    'drive': dict(verse=[(i * .5, .5, O if i in (0, 3) else M, {6: 2, 7: 1}.get(i, 0)) for i in range(8)],
                  chorus=[(0, .5, O), (.5, .5, O), (1, .5, M), (1.5, 1, O), (2.5, .5, M), (3, 1, O)],
                  bridge=[(0, 2, O), (2, 1, O), (3, 1, O)]),
}

RIFFS.update({
    'ballad': dict(verse=[(i * .5, .5, C, st) for i, st in enumerate((0, 4, 7, 9, 7, 4, 7, 4))],
                   intro=[(i * .5, .5, C, st) for i, st in enumerate((0, 4, 7, 9, 11, 9, 7, 4))],
                   chorus=[(0, 2, O), (2, 1.5, O), (3.5, .5, O)], bridge=[(0, 4, O)],
                   solo=[(0, 2, O), (2, 1.5, O), (3.5, .5, O)]),
    'shuffle': dict(verse=[(i * .5, .5, B, (7, 7, 9, 9, 7, 7, 9, 10)[i]) for i in range(8)],
                    chorus=[(0, 1.5, O), (1.5, .5, O), (2, 1.5, O), (3.5, .5, O)],
                    bridge=[(0, 1, O), (1, 1, O), (2, 2, O)]),
    'funk': dict(verse=[(0, .25, M), (.25, .25, G), (.5, .25, M), (.75, .25, M, 2), (1, .5, O), (1.5, .25, G),
                        (1.75, .25, M), (2, .25, M), (2.25, .25, G), (2.5, .25, M, 3), (2.75, .25, M, 2), (3, .5, O),
                        (3.5, .25, G), (3.75, .25, M)],
                 chorus=[(0, .5, O), (.75, .25, O), (1.5, .5, O), (2.5, .5, O), (3, .25, O), (3.25, .75, O)],
                 bridge=[(0, 1, O), (1.5, .5, O), (2, 2, O)]),
    'thrash': dict(verse=[(i * .5, .5, O if i in (0, 4) else M, {3: 1, 7: 2}.get(i, 0)) for i in range(8)],
                   chorus=[(0, 1, O), (1, 1, O), (2, .5, O), (2.5, .5, O), (3, 1, O)],
                   bridge=[(0, 1.5, O), (1.5, .5, M), (2, 1, M), (3, 1, O)]),
    'stoner': dict(verse=[(0, 1, O), (1, .5, M), (1.5, .5, M), (2, .5, M, 2), (2.5, .5, M, 3), (3, .5, O, 4),
                          (3.5, .5, M, 3)],
                   chorus=[(0, 2, O), (2, 1.5, O), (3.5, .5, M, 3)], bridge=[(0, 4, O)]),
})

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
        self.harmony = []       # a second lead a third over the first, in the last chorus
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
        own = song.get('prog')
        if isinstance(own, dict) and k in own:
            return own[k]
        if isinstance(own, list) and k not in ('intro',):
            return own
        if k not in picked:
            picked[k] = rnd.choice(prog[k])
        return picked[k]

    low = song.get('low', 40)                          # the lowest string (a low-tuned song's is under E)

    def guitar_root(degree):
        p = tonic + scale[degree % 7]
        while p > low + 12:
            p -= 12
        while p < low:
            p += 12
        return p

    lead_rhythm = {}

    def rhythm_for(kind):
        if kind not in lead_rhythm:
            # pop: a sung-like line, pushed off the beat (the hook stays easy to follow, there's more of it to play)
            pop = [[(0, .5), (.5, .5), (1, 1), (2, .5), (2.5, .5), (3, .5), (3.5, .5), (4, 1.5), (5.5, .5), (6, 1),
                    (7, 1)], LEAD_RHYTHMS[0], LEAD_RHYTHMS[3]]
            # (a slow song's lead moves in eighths and sixteenths: its beats are long)
            pool = pop if style == 'pop' else [LEAD_RHYTHMS[1], LEAD_RHYTHMS[3]] if style in ('ballad', 'stoner') \
                else LEAD_RHYTHMS[:4]
            lead_rhythm[kind] = rnd.choice(pool)
        return lead_rhythm[kind]

    # the scale's notes where the lead plays
    lead_lo, lead_hi = 57, 81
    lead_steps = [0, 3, 5, 6, 7, 10] if song.get('blues') else scale
    lead_scale = [p for p in range(lead_lo, lead_hi + 1) if (p - tonic) % 12 in lead_steps]
    pent = [0, 3, 5, 7, 10] if song['mode'] == 'minor' or song.get('blues') else [0, 2, 4, 7, 9]
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
        fast = 0.25 if style in ('hard', 'metal', 'drive', 'thrash', 'funk', 'ballad') else 0.5
        # call and answer, high
        out += melody(start_beat, 2, degrees, 'solo', True)
        first = [n[2] for n in out]
        out += melody(start_beat + 8, 2, degrees[2:] + degrees[:2], 'solo', False, motif=first)
        b = start_beat + 16
        for rep in range(max(1, bars // 8)):
            # runs: the pentatonic in rolling fours (1234 2345 3456 ...), turning at the top and coming back down, so
            # no note is struck twice running
            n = int(8 / fast)
            top = len(pent_notes) - 1
            start, up, seq = rnd.randrange(0, 3), True, []
            while len(seq) < n:
                grp = [start + k for k in range(4)] if up else [start - k for k in range(4)]
                if up and grp[-1] >= top:
                    grp = [top - 3 + k for k in range(4)]
                    up, start = False, top - 1
                elif not up and grp[-1] <= 0:
                    grp = [3 - k for k in range(4)]
                    up, start = True, 1
                else:
                    start += 1 if up else -1
                seq += grp
            for k in range(n):
                out.append((b + k * fast, fast, pent_notes[max(0, min(top, seq[k]))], False))
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
        riff = RIFFS[style].get(kind) or RIFFS[style][riffkind]
        last_section = si == len(song['form']) - 1
        for bar in range(bars):
            deg = degrees[bar % len(degrees)]
            root = guitar_root(deg)
            sc.chords.append(deg)
            b0 = beat + bar * 4
            if last_section and bar == bars - 1:
                # the end: one big chord, held, and the bass under it
                sc.rhythm.append((b0, 6, O, guitar_root(0), 0))
                sc.bass.append((b0, 6, guitar_root(0) - 12))
                continue
            walk = []
            for item in riff:
                b, l, k = item[:3]
                # a walk on the second bar of two (a line style's steps are its line: every bar)
                step = item[3] if len(item) > 3 and (bar % 2 == 1 or style in LINE_STYLES or k == C) else 0
                if k == B:
                    iv = step                                   # (a boogie's step is in semitones)
                else:
                    d0 = deg % 7
                    iv = scale[(d0 + step) % 7] + 12 * ((d0 + step) // 7) - scale[d0] if step else 0
                sc.rhythm.append((b0 + b, l, k, root, iv))
                walk.append((b, l, iv))
            # bass: by style
            broot = root - 12
            nxt = guitar_root(degrees[(bar + 1) % len(degrees)]) - 12
            if style == 'pop':
                pat = [(0, 1, broot), (1, .5, broot), (1.5, .5, broot + 12), (2, 1, broot + 7), (3, .5, broot),
                       (3.5, .5, broot + (2 if nxt > broot else -1))]
            elif style in ('metal', 'thrash') and riffkind == 'verse' or style == 'stoner':
                pat = [(b, l, broot + iv) for b, l, iv in walk]       # the bass doubles the riff
            elif style == 'shuffle':                                  # the boogie walk: up the chord and back
                pat = [(i * .5, .5, broot + w) for i, w in enumerate((0, 4, 7, 9, 10, 9, 7, 4))]
            elif style == 'funk':                                     # octaves, popped
                pat = [(b, .25, broot + w) for b, w in ((0, 0), (.5, 12), (.75, 0), (1.5, 10), (1.75, 12), (2, 0),
                                                         (2.75, 7), (3, 12), (3.5, 10), (3.75, 7))]
            elif style == 'ballad':
                pat = [(0, 2, broot), (2, 1.5, broot), (3.5, .5, broot + 7)] if kind in ('verse', 'verse2', 'intro') \
                    else [(i, 1, broot) for i in range(4)]
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
            if (kind in ('chorus', 'bridge') or style == 'ballad') and style not in ('punk', 'thrash'):
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
        # the last chorus: the lead doubled a third higher (in the key), as two guitarists do
        big = kind == 'chorus' and not any(k == 'chorus' for _, _, k in song['form'][si + 1:])
        if big:
            for b, l, p, bend in [n for n in sc.lead if beat <= n[0] < beat + bars * 4]:
                up = [q for q in lead_scale if q > p][1:2]
                if up:
                    sc.harmony.append((b, l, up[0], bend))
        # the drums
        drums(sc, rnd, style, kind, beat, bars, si == 0, last_section, big)
        beat += bars * 4
    sc.beats = beat + 8
    if song.get('swing'):
        swing(sc)
    # the count-in: four on the hi-hat
    for b in range(4):
        sc.drums.append((b, 'H', .8 if b else 1))
    sc.lead.sort()
    sc.rhythm.sort(key=lambda n: n[0])
    sc.drums.sort(key=lambda n: n[0])
    return sc


def swing(sc):
    """a shuffle: each beat's second eighth comes two thirds of the way through it (and the sixteenths with it)"""
    def sw(x):
        i = math.floor(x + 1e-9)
        f = round(x - i, 4)
        return i + {0: 0, .25: 1 / 3, .5: 2 / 3, .75: 5 / 6}.get(f, f)

    def moved(n):
        b, l = n[0], n[1]
        return (sw(b), max(1 / 6, sw(b + l) - sw(b))) + tuple(n[2:])
    sc.rhythm = [moved(n) for n in sc.rhythm]
    sc.lead = [moved(n) for n in sc.lead]
    sc.harmony = [moved(n) for n in sc.harmony]
    sc.bass = [moved(n) for n in sc.bass]
    sc.pad = [moved(n) for n in sc.pad]
    sc.drums = [(sw(b),) + tuple(n) for b, *n in sc.drums]


def drums(sc, rnd, style, kind, start, bars, first, last, big=False):
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
        elif style == 'thrash':                         # the skank: kick on the beat, snare between
            hits += [(b, 'K') for b in range(4)] + [(b + .5, 'S') for b in range(4)] + \
                    [(b, 'C' if chorus and b % 2 == 0 else 'R') for b in range(4)]
        elif style == 'funk':                           # sixteenths on the hat, the kick off the beat
            hits += [(0, 'K'), (.75, 'K'), (2.5, 'K'), (1, 'S'), (3, 'S'), (3.75, 'S')] + \
                    [(i * .25, 'HO' if chorus and i % 4 == 2 else 'H') for i in range(16)]
        elif style == 'punk':
            if chorus:
                hits += [(b, 'K') for b in range(4)] + [(b + .5, 'S') for b in range(4)] + \
                        [(b, 'R') for b in range(4)]
            else:
                hits += [(0, 'K'), (1.5, 'K'), (2, 'K'), (1, 'S'), (3, 'S')] + [(i * .5, 'H') for i in range(8)]
        else:
            kick = {'hard': [0, 2, 2.5], 'pop': [0, 1.5, 2.5], 'drive': [0, .5, 2, 2.5], 'ballad': [0, 2.5],
                    'shuffle': [0, 2], 'stoner': [0, 1.5, 2.5]}[style]
            if chorus:
                kick = kick + [1.5] if 1.5 not in kick else kick + [3.5]
            hits += [(k, 'K') for k in kick] + [(1, 'S'), (3, 'S')]
            if chorus or kind == 'solo':
                hits += [(i * .5, 'R' if style != 'pop' else 'HO' if i % 2 else 'H') for i in range(8)]
            else:
                hits += [(i * .5, 'H') for i in range(8)]
        if bar % 2 == 0 and (chorus or kind == 'solo') or bar == 0 or big:
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


# each style's amp: how hard it's driven, and where its speaker rolls the top off
AMPS = {'stoner': (34, 3000), 'funk': (3.0, 5200), 'shuffle': (5.0, 4800), 'ballad': (12, 4400)}


def clean_amp(x):
    """a clean amp: barely warmed, and a chorus's slow shimmer (a copy a dozen milliseconds behind, wavering)"""
    x = filt(x, 'highpass', 80)
    x = np.tanh(x * 1.2) / np.tanh(1.2)
    t = np.arange(len(x)) / RATE
    back = (.012 + .003 * np.sin(2 * np.pi * .8 * t)) * RATE
    x = x + .5 * np.interp(np.arange(len(x)) - back, np.arange(len(x)), x, left=0)
    return filt(x, 'lowpass', 7000)


def guitar_track(song, sc, seed, swing_ms):
    """the rhythm guitar once (twice played, one each side)"""
    bs = beat_s(song)
    rnd = random.Random(seed)
    buf = np.zeros(int((sc.beats * bs + 4) * RATE))
    clean = np.zeros(len(buf))
    notes = sc.rhythm
    for i, (b, l, kind, root, iv0) in enumerate(notes):
        nxt = notes[i + 1][0] if i + 1 < len(notes) else b + l + 2
        ring = min(nxt, b + l + (.5 if kind == O else 0)) - b
        dur = max(.05, ring * bs)
        at = b * bs + rnd.uniform(-swing_ms, swing_ms) / 1000
        if kind == C:
            # a clean picked note, left ringing into the next ones
            x = pluck(root + iv0, (l + 1) * bs, .7, .9985, seed * 5 + root + iv0)
            place(clean, at, release(x, .03), .8)
        elif kind == B:
            # the boogie: the root and the note over it, short and punchy
            x = pluck(root, min(dur, .3), .5, .99, seed * 3 + root) + .8 * pluck(root + iv0, min(dur, .3), .5, .99,
                                                                                seed * 3 + root + iv0)
            place(buf, at, release(x * np.exp(-np.arange(len(x)) / (RATE * .12)), .01), .9)
        elif kind == G:
            # a ghost: the strings damped, struck
            x = pluck(root + 12, .05, .2, .9, seed + 77) * np.exp(-np.arange(int(.05 * RATE)) / (RATE * .012))
            place(buf, at, x, .35)
        elif kind == M and iv0:
            # a walking note: one string, no fifth over it (a fifth over a scale step can fall outside the key)
            x = pluck(root + iv0, min(dur, .22), .4, .985, seed * 7 + root + iv0)
            x = x * np.exp(-np.arange(len(x)) / (RATE * .08))
            place(buf, at, release(x, .008), 1.1)
        elif kind == M:
            x = pluck(root, min(dur, .22), .35, .985, seed * 7 + root) + .6 * pluck(root + 7, min(dur, .22), .35, .985,
                                                                                  seed * 7 + root + 1)
            x = x * np.exp(-np.arange(len(x)) / (RATE * .07))
            place(buf, at, release(x, .008), .9)
        else:
            for k, iv in enumerate((0, 7, 12)):
                x = pluck(root + iv0 + iv, dur, .75, .998, seed * 13 + root * 3 + k)
                place(buf, at + k * .005, release(x), 1.0 if iv == 0 else .8)
    gain, tone = AMPS.get(song['style'], (14, 4200))
    out = amp(buf, gain, tone, tight=85)
    if np.any(clean):
        cl = clean_amp(clean)
        out = out + cl * (.6 / (np.max(np.abs(cl)) + 1e-9))
    return out


def lead_track(song, sc, notes=None, seed=1000):
    bs = beat_s(song)
    notes = sc.lead if notes is None else notes
    buf = np.zeros(int((sc.beats * bs + 4) * RATE))
    for i, (b, l, p, bend) in enumerate(notes):
        nxt = notes[i + 1][0] if i + 1 < len(notes) else b + l
        dur = max(.06, (min(nxt, b + l) - b) * bs + .02)
        x = pluck(p, dur, .85, .9996, seed + p, bend=bend, vibrato=.35 if dur > .45 else 0)
        place(buf, b * bs + (seed - 1000) * .0004, release(x, .02), .9)
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
        g = LEVEL[piece] * vel * (1 + .09 * math.sin(i * 12.9898) * math.cos(i * 4.1414))   # no two hits alike
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
    for b, l, kind, root, iv in sel:
        if kind == G:
            continue                                    # a ghost is heard, not played
        f = round(pos[root] / span * 3) if len(roots) > 1 else 1
        if kind == C:
            # an arpeggio climbs the neck as the notes climb, from low enough that its top still fits
            out.append((round(b * RES), [min(4, min(f, 1) + round(iv / 4))], sustain(l)))
            continue
        if kind == B:
            out.append((round(b * RES), [min(4, f + (1 if iv > 7 else 0))], 0))
            continue
        if iv:
            f = max(0, min(3 if kind == O else 4, f + (1 if iv <= 4 else 2)))
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


# each difficulty: the share of Expert's notes it keeps, the fewest ticks between two, its most notes a second on
# average, its top fret, and its fewest notes a second (an easy song's Easy still has something to play)
LEVELS = {'Hard': (.75, RES // 4, 4.6, 3, 2.4), 'Medium': (.5, RES // 2, 3.0, 2, 1.6), 'Easy': (.3, RES, 1.8, 2, 1.0)}


def priority(i, notes):
    """how much a note matters to the song's feel: the bar's first beat, a beat, an eighth; one held, a change of
    fret, the first after a rest"""
    t, frets, sus = notes[i]
    b = t / RES
    p = 6 if b % 4 == 0 else 4 if b % 1 == 0 else 2 if b % .5 == 0 else 0
    if sus:
        p += 3
    if len(frets) > 1:
        p += 1
    if i and notes[i - 1][1] != frets:
        p += 2
    if not i or t - notes[i - 1][0] >= RES:
        p += 3
    return p


TICK_S = [60 / 120 / RES]       # seconds a tick, in the song being charted (write_chart sets it)
GAP_S = {'Hard': .1, 'Medium': .18, 'Easy': .33}   # and the fewest seconds between two notes, however fast the song


def thin(notes, share, gap, nps, span_s, prio=priority, low_nps=0, base=None, must=(), level=None):
    """the notes that matter most, as many as the share of base (Expert's count) and the notes a second allow (at least
    low_nps), none closer than gap; must: ticks kept first (each Star Power phrase's first note)"""
    if not notes:
        return []
    base = len(notes) if base is None else base
    if level:
        gap = max(gap, math.ceil(GAP_S[level] / TICK_S[0]))
    # (the least notes a second never takes a level up to the one above: a little over its share at most)
    floor = min(int(low_nps * span_s), int(base * min(1, share + .15)))
    want = min(max(int(base * share), floor), int(nps * span_s), len(notes))
    first = {t for t in must}
    order = sorted(range(len(notes)), key=lambda i: (notes[i][0] not in first, -prio(i, notes), notes[i][0]))
    kept, taken = [], []
    for i in order:
        if len(kept) >= want:
            break
        t = notes[i][0]
        j = __import__('bisect').bisect_left(taken, t)
        if (j < len(taken) and taken[j] - t < gap) or (j and t - taken[j - 1] < gap):
            continue
        taken.insert(j, t)
        kept.append(i)
    return [notes[i] for i in sorted(kept)]


def reduce(notes, diff, max_fret_count, span_s=None, sp=()):
    """an easier chart from Expert's: the notes that matter most (LEVELS), on fewer frets, smaller chords (none on
    Easy), and a held note let go before the next"""
    if diff == 'Expert':
        return notes
    share, gap, nps, top, low = LEVELS[diff]
    if span_s is None:
        span_s = 1e9
    must = [next((t for t, f, l in notes if a <= t <= a + n), -1) for a, n in sp]
    kept = thin(notes, share, gap, nps, span_s, low_nps=low, must=must, level=diff)
    # the frets used in each two bars, onto this level's fewer: by rank when they fit, so notes that differed still do
    squeeze = {}
    for w in {t // (8 * RES) for t, f, l in kept}:
        used = sorted({f for t, fs, l in kept if t // (8 * RES) == w for f in fs})
        if len(used) <= top + 1:
            off = max(0, min(top + 1 - len(used), round(sum(used) / len(used) / 4 * (top + 1 - len(used)))))
            squeeze[w] = {f: i + off for i, f in enumerate(used)}
        else:
            squeeze[w] = {f: min(top, round(f * top / 4)) for f in used}
    out = []
    for k, (t, frets, sus) in enumerate(kept):
        fs = sorted({squeeze[t // (8 * RES)][f] for f in frets})
        if diff == 'Easy' or (diff == 'Medium' and len(fs) > 1 and fs[-1] - fs[0] > 1):
            fs = fs[:1]
        fs = fs[:max_fret_count]
        nxt = kept[k + 1][0] if k + 1 < len(kept) else t + 10 ** 9
        if sus and t + sus > nxt - RES // 4:
            sus = nxt - RES // 4 - t
        out.append((t, fs, sus if sus >= RES // 2 else 0))
    return out


DRUM_PAD = {'K': [0], 'S': [1], 'H': [2, 66], 'HO': [2, 66], 'R': [3, 67], 'C': [4, 68], 'C2': [4, 68],
            'T1': [2], 'T2': [3], 'T3': [4]}


# drums a level down: what each keeps of the kit (pads by piece), the share, the gap, the notes a second
DRUM_LEVELS = {'Hard': (.75, RES // 4, 6.0, 3.0), 'Medium': (.5, RES // 2, 4.0, 2.0), 'Easy': (.32, RES, 2.2, 1.2)}


def drum_priority(i, notes):
    t, pads, _ = notes[i]
    b = t / RES
    p = 6 if b % 4 == 0 else 4 if b % 1 == 0 else 2 if b % .5 == 0 else 0
    p += 3 * (1 in pads) + 3 * (4 in pads and 68 in pads) + 2 * (0 in pads) + len([x for x in pads if x < 8])
    return p


def easy_drums(sc, medium=False):
    """Easy's drums: kick and snare, a beat each (every other beat in a fast song), Expert's crashes, and two toms to
    end each section. medium: a beat each however fast, the ride with the kick"""
    half = 4 * TICK_S[0] * RES < 1.75 and not medium
    crashes = {round(b * RES) for b, p, v in sc.drums if p in ('C', 'C2') and b >= 4}
    out = {}
    for si, (beat, name, kind, bars) in enumerate(sc.sections):
        for bar in range(bars):
            b0 = beat + bar * 4
            hits = [(0, 'K'), (2, 'S')] if half else [(0, 'K'), (1, 'S'), (2, 'K'), (3, 'S')]
            if medium:
                hits += [(0, 'R'), (2, 'R')]
            if si == len(sc.sections) - 1 and bar == bars - 1:
                hits = [(0, 'K'), (0, 'C')]
            elif bar == bars - 1:
                hits = [(0, 'K'), (2, 'T1')] if half else [(0, 'K'), (1, 'S'), (2, 'T1'), (3, 'T3')]
            if round(b0 * RES) in crashes and (0, 'C') not in hits:
                hits.append((0, 'C'))
            for off, piece in hits:
                out.setdefault(round((b0 + off) * RES), set()).update(DRUM_PAD[piece])
    return [(t, sorted(out[t]), 0) for t in sorted(out)]


def chart_drums(sc, diff, span_s=1e9, base=None, sp=()):
    if diff == 'Easy':
        return easy_drums(sc)
    by_tick = {}
    for b, piece, vel in sc.drums:
        if b < 4:
            continue                                    # the count-in isn't played
        t = round(b * RES)
        # a level down: the kick on the eighths (Hard) or the beats; the hats and ride on the beats (Medium) or
        # gone (Easy), and Easy's toms on the beats
        if diff != 'Expert' and piece == 'K' and t % (RES // 2 if diff == 'Hard' else RES):
            continue
        if diff == 'Hard' and piece in ('H', 'HO', 'R') and t % (RES // 2):
            continue
        if diff == 'Medium' and piece in ('H', 'HO', 'R') and t % RES:
            continue
        if diff == 'Easy' and (piece in ('H', 'HO', 'R') or piece in ('T1', 'T2', 'T3') and t % RES):
            continue
        by_tick.setdefault(t, set()).update(DRUM_PAD[piece])
    out = []
    for t in sorted(by_tick):
        pads = by_tick[t]
        # a cymbal and the tom on the same pad: the cymbal; no more than two hands (one on Easy) and the kick
        hands = sorted(p for p in pads if 1 <= p <= 4)
        most = 1 if diff == 'Easy' else 2
        if len(hands) > most:
            keep = [4] if 4 in hands and 68 in pads else [1] if 1 in hands else []
            keep += [p for p in hands if p not in keep][-(most - len(keep)):] if most > len(keep) else []
            pads = {p for p in pads if p == 0 or p in keep or (p >= 66 and p - 64 in keep)}
        out.append((t, sorted(pads), 0))
    if diff != 'Expert':
        share, gap, nps, low = DRUM_LEVELS[diff]
        must = [next((t for t, f, l in out if a <= t <= a + n), -1) for a, n in sp]
        out = thin(out, share, gap, nps, span_s, drum_priority, low, base, must, diff)
        run = longest = 1
        for a, b in zip(out, out[1:]):
            run = run + 1 if a[1] == b[1] else 1
            longest = max(longest, run)
        if diff == 'Medium' and longest > 12:
            return easy_drums(sc, medium=True)          # (one hit over and over: a backbeat instead)
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
    TICK_S[0] = bs / RES
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
    def span(notes):            # seconds from a part's first note to its last
        return (notes[-1][0] - notes[0][0]) / RES * bs if len(notes) > 1 else 1

    drums_x = chart_drums(sc, 'Expert')
    for d in ('Expert', 'Hard', 'Medium', 'Easy'):
        parts.append(f'[{d}Single]\n{{\n{track_text(reduce(guitar, d, 3, span(guitar), sp), sp, solos)}\n}}')
        parts.append(f'[{d}DoubleRhythm]\n{{\n{track_text(reduce(rhythm, d, 3, span(rhythm), sp), sp, [])}\n}}')
        parts.append(f'[{d}DoubleBass]\n{{\n{track_text(reduce(bass, d, 1, span(bass), sp), sp, [])}\n}}')
        dr = chart_drums(sc, d, span(drums_x), len(drums_x), sp)
        parts.append(f'[{d}Drums]\n{{\n{track_text(dr, sp, [], True)}\n}}')
    with open(os.path.join(folder, 'notes.chart'), 'w') as f:
        f.write('\n'.join(parts) + '\n')
    counts = {'guitar': len(guitar), 'rhythm': len(rhythm), 'bass': len(bass), 'drums': len(chart_drums(sc, 'Expert'))}
    return counts


# ------------------------------------------------------------------------------------------------ the cover

def write_cover(song, path):
    """the song's cover: a picture of its own (by its motif) in its colours, the band and the title under it"""
    from PIL import Image, ImageDraw, ImageFilter, ImageFont
    rnd = random.Random(song['seed'] * 3)
    W, H = 512, 362                                     # (the picture over the name band)
    c1, c2, c3 = song['colors']
    mix = lambda a, b, k: tuple(int(a[i] * (1 - k) + b[i] * k) for i in range(3))
    img = Image.new('RGB', (W, W), c2)
    d = ImageDraw.Draw(img)
    for y in range(H):                                  # the sky: dark at the top into the song's colour
        d.line([(0, y), (W, y)], fill=mix(c2, c1, (y / H) ** 1.6 * .8))
    m = song.get('motif', 'rings')
    if m == 'road':                                     # a highway into the sunset
        d.ellipse([W / 2 - 90, H * .45 - 90, W / 2 + 90, H * .45 + 90], fill=c3)
        d.polygon([(0, H), (W, H), (W / 2 + 6, H * .55), (W / 2 - 6, H * .55)], fill=mix(c2, (0, 0, 0), .4))
        d.rectangle([0, H * .55, W, H * .58], fill=mix(c2, (0, 0, 0), .3))
        for k in range(7):
            y0, y1 = H * .6 + k * k * 4, H * .6 + k * k * 4 + 4 + k * 3
            d.polygon([(W / 2 - 1 - k, y0), (W / 2 + 1 + k, y0), (W / 2 + 2 + k * 1.5, y1), (W / 2 - 2 - k * 1.5, y1)],
                      fill=c3)
    elif m == 'crowns':                                 # paper crowns, tumbling
        for k in range(9):
            x, y, r = rnd.uniform(30, W - 30), rnd.uniform(30, H - 30), rnd.uniform(22, 60)
            pts = [(x - r, y + r * .5), (x - r, y - r * .3), (x - r * .5, y + r * .1), (x, y - r * .6),
                   (x + r * .5, y + r * .1), (x + r, y - r * .3), (x + r, y + r * .5)]
            d.polygon(pts, fill=[c3, c1, (255, 255, 255)][k % 3], outline=(40, 40, 40))
    elif m == 'shards':                                 # glass breaking, a bolt through it
        cx, cy = W * .5, H * .45
        for k in range(16):
            a0, a1 = rnd.uniform(0, 6.28), rnd.uniform(.12, .4)
            r0, r1 = rnd.uniform(30, 120), rnd.uniform(160, 380)
            pts = [(cx + r0 * math.cos(a0), cy + r0 * math.sin(a0)), (cx + r1 * math.cos(a0), cy + r1 * math.sin(a0)),
                   (cx + r1 * math.cos(a0 + a1), cy + r1 * math.sin(a0 + a1))]
            d.polygon(pts, fill=mix(c1, (255, 255, 255), rnd.uniform(0, .5)), outline=c3)
        d.polygon([(cx - 10, 10), (cx + 40, 10), (cx + 5, cy), (cx + 45, cy), (cx - 30, H - 10), (cx - 5, cy + 30),
                   (cx - 40, cy + 30)], fill=c3)
    elif m == 'stencil':                                # a sprayed star on halftone dots
        for y in range(0, H, 16):
            for x in range(0, W, 16):
                r = 6 * (1 - ((x - W / 2) ** 2 + (y - H / 2) ** 2) ** .5 / 360)
                if r > .5:
                    d.ellipse([x - r, y - r, x + r, y + r], fill=c1)
        pts = [(W / 2 + (150 if k % 2 == 0 else 60) * math.cos(k * math.pi / 5 - math.pi / 2),
                H / 2 + (150 if k % 2 == 0 else 60) * math.sin(k * math.pi / 5 - math.pi / 2)) for k in range(10)]
        d.polygon(pts, fill=c3, outline=(0, 0, 0))
    elif m == 'gauge':                                  # the speedometer at midnight, the needle past the red
        d.ellipse([W - 130, 30, W - 50, 110], fill=(240, 240, 220))
        d.ellipse([W - 110, 22, W - 40, 92], fill=mix(c2, c1, .3))
        cx, cy, r = W / 2, H * .9, 210
        d.arc([cx - r, cy - r, cx + r, cy + r], 200, 340, fill=c1, width=18)
        d.arc([cx - r, cy - r, cx + r, cy + r], 310, 340, fill=c3, width=18)
        for k in range(15):
            a = math.radians(200 + k * 10)
            d.line([(cx + (r - 40) * math.cos(a), cy + (r - 40) * math.sin(a)),
                    (cx + (r - 15) * math.cos(a), cy + (r - 15) * math.sin(a))], fill=(230, 230, 230), width=4)
        a = math.radians(328)
        d.line([(cx, cy), (cx + (r - 20) * math.cos(a), cy + (r - 20) * math.sin(a))], fill=c3, width=8)
    elif m == 'aurora':                                 # the northern lights over the mountains, stars
        for k in range(60):
            x, y = rnd.uniform(0, W), rnd.uniform(0, H * .6)
            d.ellipse([x - 1.2, y - 1.2, x + 1.2, y + 1.2], fill=(255, 255, 255))
        for band in range(3):
            for x in range(0, W, 2):
                y = H * (.25 + band * .1) + 30 * math.sin(x / 70 + band * 2)
                d.line([(x, y), (x, y + 90)], fill=mix(c1, c3, band / 2.5), width=2)
        d.polygon([(0, H), (0, H * .75), (110, H * .5), (200, H * .72), (300, H * .45), (420, H * .7), (W, H * .58),
                   (W, H)], fill=mix(c2, (0, 0, 0), .5))
    elif m == 'poles':                                  # a gravel road, the telephone poles going away along it
        d.ellipse([W * .65, H * .2, W * .65 + 110, H * .2 + 110], fill=c3)
        d.rectangle([0, H * .62, W, H], fill=mix(c1, (0, 0, 0), .35))
        d.polygon([(W * .2, H), (W * .62, H), (W * .52, H * .62), (W * .48, H * .62)], fill=mix(c1, c3, .4))
        for k in range(6):
            x, top, h = W * .7 + k * k * 14, H * .62 - 150 / (k + 1), 150 / (k + 1)
            d.rectangle([x, top - h, x + 7 - k, H * .62 + 10 * k], fill=(30, 20, 10))
            d.rectangle([x - 20 + k * 2, top - h + 6, x + 27 - k * 3, top - h + 10], fill=(30, 20, 10))
    elif m == 'disco':                                  # a lit floor, zigzags over it
        for gy in range(5):
            for gx in range(8):
                y0 = H * .55 + gy * gy * 6 + gy * 14
                x0, x1 = W / 2 + (gx - 4) * (30 + gy * 18), W / 2 + (gx - 3) * (30 + gy * 18)
                d.rectangle([x0 + 2, y0, x1 - 2, y0 + 10 + gy * 8], fill=[c1, c3, (255, 255, 255)][(gx + gy) % 3])
        for k in range(3):
            pts = [(x, 40 + k * 50 + (25 if (x // 40) % 2 else 0)) for x in range(0, W + 40, 40)]
            d.line(pts, fill=[c3, c1, (255, 255, 255)][k], width=8)
    elif m == 'saw':                                    # a saw blade, spinning
        cx, cy, r = W / 2, H / 2, 140
        pts = []
        for k in range(48):
            a = k * math.pi / 24
            rr = r if k % 2 == 0 else r - 26
            pts.append((cx + rr * math.cos(a + (.08 if k % 2 == 0 else 0)), cy + rr * math.sin(a)))
        d.polygon(pts, fill=(200, 200, 205), outline=(40, 40, 40))
        d.ellipse([cx - 28, cy - 28, cx + 28, cy + 28], fill=c1)
        for k in range(5):
            a = rnd.uniform(0, 6.28)
            d.line([(cx + 40 * math.cos(a), cy + 40 * math.sin(a)), (cx + 100 * math.cos(a), cy + 100 * math.sin(a))],
                   fill=c1, width=6)
    elif m == 'dunes':                                  # a huge sun on the dunes
        d.ellipse([W / 2 - 140, H * .1, W / 2 + 140, H * .1 + 280], fill=c3)
        for k in range(4):
            pts = [(x, H * (.6 + k * .1) + 22 * math.sin(x / (90 + k * 30) + k)) for x in range(0, W + 8, 8)]
            d.polygon(pts + [(W, H), (0, H)], fill=mix(c1, c2, .2 + k * .2))
    else:                                               # rings and rays
        cx, cy = W * .5, H * .45
        for r in (70, 120, 180):
            d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=c3, width=3)
    img = img.filter(ImageFilter.SMOOTH)
    d = ImageDraw.Draw(img)
    fonts = '/usr/share/fonts/truetype/dejavu/'
    big = ImageFont.truetype(fonts + 'DejaVuSans-Bold.ttf', 46)
    small = ImageFont.truetype(fonts + 'DejaVuSans-Bold.ttf', 30)

    def text(t, y, font, fill):
        size, f = font.size, font
        while d.textlength(t, font=f) > W - 50 and size > 14:
            size -= 2
            f = ImageFont.truetype(f.path, size)
        x = (W - d.textlength(t, font=f)) / 2
        d.text((x + 3, y + 3), t, font=f, fill=(0, 0, 0))
        d.text((x, y), t, font=f, fill=fill)

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
        subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-i', w, '-c:a', 'libvorbis', '-q:a', '2', path],
                       check=True)


# how hard each part plays in each kind of section (rhythm guitars, drums, bass, lead, pad): the verses back off for
# the choruses to hit, the bridge drops, the solo's backing makes room for the lead
ENERGY = {'intro': (.8, .82, .88, 1, 1), 'verse': (.58, .7, .82, .9, 1), 'verse2': (.62, .74, .84, .9, 1),
          'chorus': (1, 1, 1, 1, 1), 'bridge': (.48, .6, .75, 1, 1), 'solo': (.6, .88, .9, 1.12, 1),
          'outro': (1, 1, 1, 1, 1)}


def energy_curve(sc, bs, n, which):
    g = np.ones(n)
    last_chorus = max((b for b, nm, k, bars in sc.sections if k == 'chorus'), default=-1)
    for beat, name, kind, bars in sc.sections:
        a, b = int(beat * bs * RATE), int((beat + bars * 4) * bs * RATE)
        lift = 1.14 if beat == last_chorus and which < 3 else 1     # the last chorus, the biggest
        g[a:b] = ENERGY[kind][which] * lift
    return ndimage.uniform_filter1d(g, int(.08 * RATE))            # changing over a moment, not a click


def make(song, out, pack=False, charts_only=False):
    sc = compose(song)
    # (packed for the game, each under its key: the Vita's packager can't take a file name with spaces)
    folder = os.path.join(out, song['key'] if pack else f'{song["artist"]} - {song["name"]}')
    os.makedirs(folder, exist_ok=True)
    counts = write_chart(song, sc, folder)
    if charts_only:
        print(f'{folder}: chart, notes {counts}')
        return folder
    # the band, each on its own and then together
    gl = guitar_track(song, sc, 1, 6)
    gr = guitar_track(song, sc, 2, 6)
    lead = lead_track(song, sc)
    harm = lead_track(song, sc, sc.harmony, 1003) if sc.harmony else np.zeros(len(lead))
    bass = bass_track(song, sc)
    pad = pad_track(song, sc)
    dl, dr, room = drum_tracks(song, sc)
    n = min(len(gl), len(gr), len(lead), len(bass), len(pad), len(dl))

    def fit(x):
        return x[:n] / (np.max(np.abs(x[:n])) + 1e-9)

    gl, gr, lead, bass, pad, harm = fit(gl), fit(gr), fit(lead), fit(bass), fit(pad), fit(harm)
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
    # then each section as hard as it should be (the levels above are set at full strength)
    bs = beat_s(song)
    e = [energy_curve(sc, bs, n, k) for k in range(5)]
    gl, gr, lead, bass, pad = gl * e[0], gr * e[0], lead * e[3], bass * e[2], pad * e[4]
    # the second lead under the first, from the other side
    hl, hr = harm * g_lead * .62, harm * g_lead * .3
    dl, dr, room = dl * e[1], dr * e[1], room * e[1]
    lead_room = reverb(lead * g_lead, 1.6, 7)
    room_l = reverb(room * g_drum, 1.1, 9) + reverb(pad * g_pad, 2.0, 11)
    room_r = reverb(room * g_drum, 1.1, 10) + reverb(pad * g_pad, 2.0, 12)
    guitars = np.stack([gl * g_rhy * .95 + gr * g_rhy * .25 + lead * g_lead * .8 + lead_room * .35 + hl,
                        gr * g_rhy * .95 + gl * g_rhy * .25 + lead * g_lead * .9 + lead_room * .35 + hr], 1)
    rest = np.stack([dl * g_drum + bass * g_bass + pad * g_pad * .9 + room_l * .3,
                     dr * g_drum + bass * g_bass + pad * g_pad * 1.1 + room_r * .3], 1)
    rest = signal.sosfilt(butter('highpass', 38, 4), rest, axis=0)     # nothing under the kick's and bass's notes
    # the whole mix squeezed a little (the same gain on both halves, so together they're the mix), to -1 dB
    mix = guitars + rest
    env = np.sqrt(signal.sosfilt(butter('lowpass', 8), np.mean(mix ** 2, 1)).clip(1e-12))
    thr = .34                   # (only the loudest moments: under it, the verses and choruses keep their difference)
    gain = np.where(env > thr, (thr / env) ** (1 - 1 / 2.0), 1.0)[:, None]
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
                f'diff_drums = {t["drums"]}\ndiff_band = {round(sum(t.values()) / 4)}\ninstrumental = 1\n'
                f'loading_phrase = An original song made for GH Vita: play it, share it, keep it.\n')
    print(f'{folder}: {length:.0f} s, notes {counts}')
    return folder


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('keys', nargs='*')
    ap.add_argument('--pack', action='store_true',
                    help="for the game's assets/starter: folders by key, and list.txt (key, the songs folder's name, "
                         "the version; tabs between)")
    ap.add_argument('--charts', action='store_true', help='the charts only (notes.chart), not the music')
    a = ap.parse_args()
    for s in SONGS:
        if not a.keys or s['key'] in a.keys:
            make(s, a.out, a.pack, a.charts)
    if a.pack:
        with open(os.path.join(a.out, 'list.txt'), 'w') as f:
            for s in SONGS:
                if os.path.isdir(os.path.join(a.out, s['key'])):
                    f.write(f'{s["key"]}\t{s["artist"]} - {s["name"]}\t{STARTER_VERSION}\n')
