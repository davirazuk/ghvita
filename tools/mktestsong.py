#!/usr/bin/env python3
"""Makes small original Clone Hero songs for the tests: mktestsong.py OUT_DIR [--count N] [--seconds S]
Each is a folder (song.ini, notes.chart, song.ogg) with guitar, bass, keys and drums on all four difficulties (and one MIDI song with guitar and vocals), sections,
Star Power phrases and a solo, and audio synthesized from the chart (a tone per note over a click), so the bot has
songs to play on a machine with no library. Needs numpy and ffmpeg."""
import argparse, os, random, subprocess, tempfile, wave
import numpy as np

RES = 192                     # ticks per beat
RATE = 44100
FREQ = [261.6, 293.7, 329.6, 392.0, 440.0]   # one pitch per fret

ap = argparse.ArgumentParser()
ap.add_argument('out')
ap.add_argument('--count', type=int, default=2)
ap.add_argument('--seconds', type=int, default=45)
a = ap.parse_args()


def make(idx):
    rnd = random.Random(1000 + idx)
    bpm = [120, 150, 96, 135][idx % 4]
    beats = int(a.seconds * bpm / 60) // 4 * 4
    name, artist = f'Test Song {idx + 1}', 'GH Vita Tests'
    folder = os.path.join(a.out, f'{artist} - {name}')
    os.makedirs(folder, exist_ok=True)

    # the expert lead: a phrase per bar, chords now and then, a sustain at the end of each 4-bar line
    lead = []                             # (tick, frets bitmask, length)
    for bar in range(1, beats // 4):
        step = rnd.choice([1, 2, 2, 4])   # notes per beat
        for k in range(4 * step):
            if rnd.random() < 0.15:
                continue
            tick = bar * 4 * RES + k * RES // step
            f = 1 << rnd.randrange(5)
            if rnd.random() < 0.12:
                f |= 1 << rnd.randrange(5)
            lead.append((tick, f, 0))
        if bar % 4 == 3:
            lead.append(((bar + 1) * 4 * RES - RES, 1 << rnd.randrange(5), RES // 2))
    lead.sort()

    def reduce(notes, keep, top):         # easier parts: fewer notes, fewer frets
        out = []
        for i, (t, f, l) in enumerate(notes):
            if t % (RES * keep):
                continue
            frets = [b for b in range(5) if f >> b & 1][:1]
            out.append((t, 1 << min(frets[0], top - 1), l))
        return out

    parts = {'Expert': lead, 'Hard': reduce(lead, 1, 4) if lead else [],
             'Medium': reduce(lead, 2, 3), 'Easy': reduce(lead, 4, 3)}
    bars = beats // 4
    sp = [(b * 4 * RES, 4 * RES) for b in range(4, bars - 2, 8)]
    solo = (bars // 2 * 4 * RES, bars // 2 * 4 * RES + 8 * RES * 4) if bars > 20 else None

    def track(notes, drums=False):
        lines = []
        for t, f, l in notes:
            for b in range(5):
                if f >> b & 1:
                    lines.append((t, f'N {b} {0 if drums else l}'))
        for t, l in sp:
            lines.append((t, f'S 2 {l}'))
        if solo and not drums:
            lines += [(solo[0], 'E solo'), (solo[1], 'E soloend')]
        lines.sort(key=lambda x: x[0])
        return '\n'.join(f'  {t} = {s}' for t, s in lines)

    drums = []
    for beat in range(4, beats):
        t = beat * RES
        drums.append((t, 1 if beat % 2 == 0 else 2, 0))        # kick on 1 and 3, snare on 2 and 4 (red)
        drums.append((t + RES // 2, 4, 0))                      # yellow (hi-hat) on the off-beats

    secs = ['Intro', 'Verse 1', 'Chorus 1', 'Verse 2', 'Chorus 2', 'Solo', 'Bridge', 'Outro']
    ev = '\n'.join(f'  {i * bars // len(secs) * 4 * RES} = E "section {s}"' for i, s in enumerate(secs))
    chart = [f'[Song]\n{{\n  Name = "{name}"\n  Artist = "{artist}"\n  Resolution = {RES}\n  Offset = 0\n}}',
             f'[SyncTrack]\n{{\n  0 = TS 4\n  0 = B {bpm * 1000}\n}}', f'[Events]\n{{\n{ev}\n}}']
    for d, notes in parts.items():
        chart.append(f'[{d}Single]\n{{\n{track(notes)}\n}}')
        chart.append(f'[{d}DoubleBass]\n{{\n{track(reduce(notes, 1, 3) if d == "Expert" else notes)}\n}}')
        chart.append(f'[{d}Keyboard]\n{{\n{track(reduce(notes, 2, 5) if d == "Expert" else notes)}\n}}')
        chart.append(f'[{d}Drums]\n{{\n{track(drums if d in ("Expert", "Hard") else drums[::2], True)}\n}}')
    open(os.path.join(folder, 'notes.chart'), 'w').write('\n'.join(chart) + '\n')

    # audio: a click on every beat and a short tone for each expert note
    length = (beats + 4) * 60 / bpm
    buf = np.zeros(int(length * RATE), np.float32)
    tick_s = 60 / bpm / RES
    click = np.sin(2 * np.pi * 1000 * np.arange(800) / RATE) * np.linspace(1, 0, 800) * 0.3
    for beat in range(beats):
        s = int(beat * RES * tick_s * RATE)
        buf[s:s + 800] += click[:len(buf[s:s + 800])]
    for t, f, l in lead:
        s = int(t * tick_s * RATE)
        n = int(max(0.12, l * tick_s) * RATE)
        env = np.exp(-np.arange(n) / (n / 3))
        for b in range(5):
            if f >> b & 1:
                seg = buf[s:s + n]
                seg += (np.sin(2 * np.pi * FREQ[b] * np.arange(len(seg)) / RATE) * env[:len(seg)] * 0.25)
    pcm = (np.clip(buf, -1, 1) * 32000).astype('<i2')
    with tempfile.TemporaryDirectory() as tmp:
        w = os.path.join(tmp, 'a.wav')
        with wave.open(w, 'wb') as f:
            f.setnchannels(1); f.setsampwidth(2); f.setframerate(RATE); f.writeframes(pcm.tobytes())
        subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-i', w, '-c:a', 'libvorbis', '-q:a', '2',
                        os.path.join(folder, 'song.ogg')], check=True)
    open(os.path.join(folder, 'song.ini'), 'w').write(
        f'[song]\nname = {name}\nartist = {artist}\nalbum = Tests\ngenre = Test\nyear = 2026\ncharter = mktestsong\n'
        f'song_length = {int(length * 1000)}\ndiff_guitar = {idx % 7}\ndiff_bass = 2\ndiff_drums = 3\n')
    print(folder)


def vlq(n):
    out = [n & 0x7f]
    n >>= 7
    while n:
        out.insert(0, (n & 0x7f) | 0x80)
        n >>= 7
    return bytes(out)


def track(events):
    """events: (tick, bytes) -> an MTrk chunk, delta-timed, ended"""
    out, last = b'', 0
    for tick, ev in sorted(events, key=lambda e: (e[0], e[1][0] == 0x80 and -1 or 0)):
        out += vlq(tick - last) + ev
        last = tick
    out += vlq(0) + b'\xff\x2f\x00'
    return b'MTrk' + len(out).to_bytes(4, 'big') + out


def name(text):
    return b'\xff\x03' + vlq(len(text)) + text.encode()


def make_vocal_song():
    """a MIDI song with guitar and a sung part (lyrics, phrases), for the singing tests"""
    rnd = random.Random(77)
    bpm, beats = 110, int(a.seconds * 110 / 60) // 4 * 4
    nm, artist = 'Test Song Vocals', 'GH Vita Tests'
    folder = os.path.join(a.out, f'{artist} - {nm}')
    os.makedirs(folder, exist_ok=True)
    T = 480
    tempo = track([(0, b'\xff\x51\x03' + (60000000 // bpm).to_bytes(3, 'big')), (0, name('TEMPO'))])
    gt = [(0, name('PART GUITAR'))]
    for beat in range(4, beats):
        f = rnd.randrange(5)
        gt += [(beat * T, bytes([0x90, 96 + f, 100])), (beat * T + T // 4, bytes([0x80, 96 + f, 0]))]
    words = 'oh the road goes on and on we sing it loud we sing it strong'.split()
    vx = [(0, name('PART VOCALS'))]
    tunes = []
    for ph in range(2, beats // 4 - 1, 2):
        start = ph * 4 * T
        vx += [(start, bytes([0x90, 105, 100])), (start + 6 * T, bytes([0x80, 105, 0]))]
        for k in range(6):
            t0, pitch, w = start + k * T, 55 + rnd.randrange(12), words[(ph + k) % len(words)]
            vx += [(t0, b'\xff\x05' + vlq(len(w)) + w.encode()), (t0, bytes([0x90, pitch, 100])),
                   (t0 + T * 3 // 4, bytes([0x80, pitch, 0]))]
            tunes.append((t0, pitch))
    ev = [(0, name('EVENTS'))] + [(i * beats // 4 * T, b'\xff\x01' + vlq(len(t)) + t.encode())
                                  for i, t in enumerate(['[section Intro]', '[section Verse]', '[section Chorus]', '[section Outro]'])]
    mid = b'MThd' + (6).to_bytes(4, 'big') + (1).to_bytes(2, 'big') + (4).to_bytes(2, 'big') + T.to_bytes(2, 'big')
    mid += tempo + track(gt) + track(vx) + track(ev)
    open(os.path.join(folder, 'notes.mid'), 'wb').write(mid)
    # the audio: the click and the sung line as a plain voice-like tone (so autoplay's detector hears pitches)
    length = (beats + 4) * 60 / bpm
    buf = np.zeros(int(length * RATE), np.float32)
    sec = 60 / bpm / T
    for t0, pitch in tunes:
        s0, n = int(t0 * sec * RATE), int(T * 3 / 4 * sec * RATE)
        f = 440 * 2 ** ((pitch - 69) / 12)
        x = np.arange(n) / RATE
        seg = buf[s0:s0 + n]
        seg += (0.3 * np.sin(2 * np.pi * f * x) + 0.1 * np.sin(4 * np.pi * f * x))[:len(seg)]
    pcm = (np.clip(buf, -1, 1) * 32000).astype('<i2')
    with tempfile.TemporaryDirectory() as tmp:
        w = os.path.join(tmp, 'a.wav')
        with wave.open(w, 'wb') as f:
            f.setnchannels(1); f.setsampwidth(2); f.setframerate(RATE); f.writeframes(pcm.tobytes())
        subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-i', w, '-c:a', 'libvorbis', '-q:a', '2',
                        os.path.join(folder, 'song.ogg')], check=True)
    open(os.path.join(folder, 'song.ini'), 'w').write(
        f'[song]\nname = {nm}\nartist = {artist}\nalbum = Tests\ngenre = Test\nyear = 2026\ncharter = mktestsong\n'
        f'song_length = {int(length * 1000)}\ndiff_guitar = 1\ndiff_vocals = 1\n')
    print(folder)


for i in range(a.count):
    make(i)
make_vocal_song()
