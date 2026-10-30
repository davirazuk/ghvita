#!/usr/bin/env python3
"""Pack Clone Hero / YARG songs for GH Vita.

usage: pack.py [-o OUT] [-j JOBS] [--force] SOURCE [SOURCE ...]

A SOURCE is a song folder (song.ini + notes.chart or notes.mid + audio), a .sng file, or any folder holding those
(searched recursively). Each song becomes OUT/<Artist - Title>/ with:
  song.ini     the metadata the Vita reads (name, artist, album, year, charter, length, preview, parts, stems)
  notes.vhc    the 5-fret parts (guitar, bass, rhythm, co-op, keys) on every difficulty, already timed in microseconds,
               with hammer-ons, taps, open notes, Star Power phrases and solos worked out, plus the tempo map,
               beat lines and sections
  backing.ogg  every stem that isn't a playable part, mixed (48 kHz Vorbis)
  guitar.ogg, bass.ogg, rhythm.ogg, keys.ogg   the playable parts' own stems, when the song has them (a miss mutes yours)
  album.jpg    the cover, 256x256
Songs already packed (same source size and date) are skipped unless --force.
"""
import argparse, concurrent.futures, hashlib, io, json, os, re, shutil, struct, subprocess, sys, tempfile

AUDIO_EXT = ('.ogg', '.opus', '.mp3', '.wav', '.flac')
DIFFS = ('Easy', 'Medium', 'Hard', 'Expert')
# playable 5-fret parts: (id, .chart suffix, .mid track name, stem)
PARTS = [
    (0, 'Single', 'PART GUITAR', 'guitar'),
    (1, 'DoubleBass', 'PART BASS', 'bass'),
    (2, 'DoubleRhythm', 'PART RHYTHM', 'rhythm'),
    (3, 'DoubleGuitar', 'PART GUITAR COOP', 'guitar'),
    (4, 'Keyboard', 'PART KEYS', 'keys'),
    (5, 'Drums', 'PART DRUMS', 'drums'),
]
OPEN = 0x20
F_HOPO, F_TAP, F_SP, F_SP_END = 1, 2, 4, 8


# ---------------------------------------------------------------------------------------------------------- sources

def sng_unmask(buf, mask):
    import numpy as np
    a = np.frombuffer(buf, dtype=np.uint8)
    i = np.arange(len(a), dtype=np.uint64)
    m = np.frombuffer(bytes(mask), dtype=np.uint8)[(i % 16).astype(np.int64)]
    return (a ^ m ^ (i & 0xFF).astype(np.uint8)).tobytes()


class Source:
    """A song's files, from a folder or a .sng package: names are lowercased."""

    def __init__(self, path):
        self.path = path
        self.meta = {}
        self.files = {}         # lower name -> path on disk, or (offset, size) inside the .sng
        if os.path.isdir(path):
            for n in os.listdir(path):
                p = os.path.join(path, n)
                if os.path.isfile(p):
                    self.files[n.lower()] = p
            self.sng = None
        else:
            d = open(path, 'rb').read()
            if d[:6] != b'SNGPKG':
                raise ValueError('not a .sng file')
            self.sng = d
            self.mask = d[10:26]
            o = 26
            mlen, mcount = struct.unpack_from('<QQ', d, o); o += 16
            for _ in range(mcount):
                kl = struct.unpack_from('<i', d, o)[0]; o += 4
                k = d[o:o + kl].decode('utf-8', 'replace'); o += kl
                vl = struct.unpack_from('<i', d, o)[0]; o += 4
                v = d[o:o + vl].decode('utf-8', 'replace'); o += vl
                self.meta[k.lower()] = v
            flen, fcount = struct.unpack_from('<QQ', d, o); o += 16
            for _ in range(fcount):
                nl = d[o]; o += 1
                n = d[o:o + nl].decode('utf-8', 'replace'); o += nl
                sz, idx = struct.unpack_from('<QQ', d, o); o += 16
                self.files[n.lower()] = (idx, sz)

    def has(self, name):
        return name in self.files

    def read(self, name):
        f = self.files[name]
        if isinstance(f, str):
            return open(f, 'rb').read()
        idx, sz = f
        return sng_unmask(self.sng[idx:idx + sz], self.mask)

    def disk_path(self, name, tmp):
        """a path ffmpeg can open"""
        f = self.files[name]
        if isinstance(f, str):
            return f
        p = os.path.join(tmp, name)
        with open(p, 'wb') as o:
            o.write(self.read(name))
        return p

    def stamp(self):
        st = os.stat(self.path)
        if os.path.isdir(self.path):
            sizes = sorted((n, os.path.getsize(p), int(os.path.getmtime(p))) for n, p in self.files.items())
            return hashlib.md5(repr(sizes).encode()).hexdigest()
        return '%d-%d' % (st.st_size, int(st.st_mtime))

    def ini(self):
        if self.sng is not None:
            return dict(self.meta)
        if 'song.ini' not in self.files:
            return {}
        raw = self.read('song.ini')
        for enc in ('utf-8-sig', 'cp1252', 'latin-1'):
            try:
                text = raw.decode(enc)
                break
            except UnicodeDecodeError:
                pass
        ini = {}
        for line in text.splitlines():
            line = line.strip()
            if not line or line.startswith(('[', ';', '#', '//')) or '=' not in line:
                continue
            k, v = line.split('=', 1)
            k = k.strip().lower()
            if k not in ini:
                ini[k] = v.strip()
        return ini


def find_sources(paths):
    for p in paths:
        if os.path.isfile(p):
            if p.lower().endswith('.sng'):
                yield p
            continue
        for root, dirs, files in os.walk(p):
            dirs.sort()
            low = [f.lower() for f in files]
            if 'notes.chart' in low or 'notes.mid' in low:
                yield root
            for f in sorted(files):
                if f.lower().endswith('.sng'):
                    yield os.path.join(root, f)


# ----------------------------------------------------------------------------------------------------------- charts

class Tempo:
    """tick -> seconds through the tempo map"""

    def __init__(self, res, tempos):
        self.res = res
        tempos = sorted(tempos)
        if not tempos or tempos[0][0] != 0:
            tempos.insert(0, (0, 500000))
        self.points = []        # (tick, seconds, us per quarter)
        sec, last_tick, last_us = 0.0, 0, tempos[0][1]
        for tick, us in tempos:
            sec += (tick - last_tick) * last_us / 1e6 / res
            self.points.append((tick, sec, us))
            last_tick, last_us = tick, us

    def seconds(self, tick):
        lo, hi = 0, len(self.points) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if self.points[mid][0] <= tick:
                lo = mid
            else:
                hi = mid - 1
        t, s, us = self.points[lo]
        return s + (tick - t) * us / 1e6 / self.res


class Track:
    def __init__(self):
        self.notes = {}         # tick -> {fret: length}, fret 0-4, 7 = open
        self.forced = []        # .chart: ticks with the HOPO flip;  .mid: (start, end, on)
        self.taps = []          # (start, end)
        self.sp = []            # (start, end)
        self.solos = []         # (start, end)


def parse_chart(text):
    sections = {}
    name = None
    body = None
    for line in text.splitlines():
        line = line.strip()
        if line.startswith('[') and line.endswith(']'):
            name = line[1:-1]
            body = None
        elif line == '{':
            body = sections.setdefault(name, [])
        elif line == '}':
            body = None
        elif body is not None and '=' in line:
            k, v = line.split('=', 1)
            body.append((k.strip(), v.strip()))
    song = dict((k.lower(), v.strip('"')) for k, v in sections.get('Song', []))
    res = int(float(song.get('resolution', 192)))
    tempos, sigs = [], []
    for k, v in sections.get('SyncTrack', []):
        parts = v.split()
        if not parts or not k.isdigit():
            continue
        if parts[0] == 'B':
            tempos.append((int(k), int(round(60000000000 / max(1, int(parts[1]))))))
        elif parts[0] == 'TS':
            num = int(parts[1])
            den = 2 ** int(parts[2]) if len(parts) > 2 else 4
            sigs.append((int(k), num, den))
    names = []
    for k, v in sections.get('Events', []):
        m = re.match(r'E\s+"?section\s+(.*?)"?$', v)
        if m and k.isdigit():
            names.append((int(k), m.group(1)))
    tracks = {}
    for pid, suffix, _, _ in PARTS:
        for d, dn in enumerate(DIFFS):
            body = sections.get(dn + suffix)
            if not body:
                continue
            t = Track()
            solo = None
            for k, v in body:
                if not k.isdigit():
                    continue
                tick = int(k)
                parts = v.split()
                if parts[0] == 'N' and len(parts) >= 3:
                    fret, length = int(parts[1]), int(parts[2])
                    if fret <= 4 or fret == 7:
                        t.notes.setdefault(tick, {})[fret] = length
                    elif fret == 5:
                        t.forced.append(tick)
                    elif fret == 6:
                        t.taps.append((tick, tick + 1))
                elif parts[0] == 'S' and len(parts) >= 3 and parts[1] == '2':
                    t.sp.append((tick, tick + max(1, int(parts[2]))))
                elif parts[0] == 'E' and len(parts) >= 2:
                    ev = parts[1].strip('"')
                    if ev == 'solo':
                        solo = tick
                    elif ev == 'soloend' and solo is not None:
                        t.solos.append((solo, tick + 1))
                        solo = None
            if t.notes:
                tracks[(pid, d)] = t
    return 'chart', res, tempos, sigs, names, tracks


def vlq(d, o):
    v = 0
    while True:
        b = d[o]; o += 1
        v = (v << 7) | (b & 0x7F)
        if not b & 0x80:
            return v, o


def parse_mid(d, ini):
    if d[:4] != b'MThd':
        raise ValueError('not a MIDI file')
    hl = struct.unpack_from('>I', d, 4)[0]
    fmt, ntr, div = struct.unpack_from('>HHH', d, 8)
    if div & 0x8000:
        raise ValueError('SMPTE time division')
    res = div
    o = 8 + hl
    tempos, sigs, names = [], [], []
    raw_tracks = []
    for _ in range(ntr):
        if d[o:o + 4] != b'MTrk':
            break
        tl = struct.unpack_from('>I', d, o + 4)[0]
        o += 8
        end = min(o + tl, len(d))
        tick = 0
        status = 0
        tname = None
        notes = {}              # pitch -> list of [start, end]
        open_ = {}              # pitch -> stack of starts
        sysex = []              # (tick, diff, type, on)
        texts = []
        try:
            while o < end:
                delta, o = vlq(d, o)
                tick += delta
                b = d[o]
                if b == 0xFF:
                    typ = d[o + 1]
                    ln, o = vlq(d, o + 2)
                    data = d[o:o + ln]; o += ln
                    if typ == 0x03 and tname is None:
                        tname = data.decode('latin-1').strip()
                    elif typ == 0x51 and ln == 3:
                        tempos.append((tick, (data[0] << 16) | (data[1] << 8) | data[2]))
                    elif typ == 0x58 and ln >= 2:
                        sigs.append((tick, data[0], 2 ** data[1]))
                    elif typ in (0x01, 0x05, 0x06, 0x07):
                        texts.append((tick, data.decode('utf-8', 'replace')))
                    continue
                if b in (0xF0, 0xF7):
                    ln, o = vlq(d, o + 1)
                    data = d[o:o + ln]; o += ln
                    # Phase Shift: 50 53 00 00 <difficulty|FF> <01 open / 04 tap> <on> F7
                    if len(data) >= 7 and data[0] == 0x50 and data[1] == 0x53 and data[2] == 0:
                        sysex.append((tick, data[4], data[5], data[6]))
                    continue
                if b & 0x80:
                    status = b
                    o += 1
                kind = status & 0xF0
                if kind in (0x80, 0x90):
                    pitch, vel = d[o], d[o + 1]
                    o += 2
                    if kind == 0x90 and vel > 0:
                        open_.setdefault(pitch, []).append(tick)
                    else:
                        st = open_.get(pitch)
                        if st:
                            notes.setdefault(pitch, []).append((st.pop(0), tick))
                elif kind in (0xA0, 0xB0, 0xE0):
                    o += 2
                elif kind in (0xC0, 0xD0):
                    o += 1
                else:
                    raise ValueError('bad MIDI status %02x' % status)
        except IndexError:
            pass
        for pitch, starts in open_.items():
            for s in starts:
                notes.setdefault(pitch, []).append((s, s + 1))
        o = end
        raw_tracks.append((tname, notes, sysex, texts))

    sp_note = 103 if str(ini.get('multiplier_note', ini.get('star_power_note', ''))).strip() == '103' else 116
    tracks = {}
    for tname, notes, sysex, texts in raw_tracks:
        if tname == 'EVENTS':
            for tick, text in texts:
                m = re.match(r'\[(?:section|prc)[ _](.*)\]$', text.strip())
                if m:
                    names.append((tick, m.group(1)))
            continue
        part = [p for p in PARTS if p[2] == tname]
        if not part:
            continue
        pid = part[0][0]
        enhanced_opens = any('ENHANCED_OPENS' in t for _, t in texts)
        for di in range(4):
            base = 60 + 12 * di
            t = Track()
            for fret in range(5):
                for s, e in notes.get(base + fret, []):
                    t.notes.setdefault(s, {})[fret] = e - s
            if enhanced_opens:
                for s, e in notes.get(base - 1, []):
                    t.notes.setdefault(s, {})[7] = e - s
            if not t.notes:
                continue
            for s, e in notes.get(base + 5, []):
                t.forced.append((s, e, True))
            for s, e in notes.get(base + 6, []):
                t.forced.append((s, e, False))
            t.taps = list(notes.get(104, []))
            t.sp = list(notes.get(sp_note, []))
            if sp_note == 116:
                t.solos = list(notes.get(103, []))
            # Phase Shift sysex phrases: open notes (green turns open) and taps
            for typ in (1, 4):
                start = None
                phrases = []
                for tick, diff, ty, on in sorted(sysex):
                    if ty != typ or diff not in (di, 0xFF):
                        continue
                    if on and start is None:
                        start = tick
                    elif not on and start is not None:
                        phrases.append((start, tick + 1))
                        start = None
                if typ == 4:
                    t.taps += phrases
                else:
                    for tick, frets in t.notes.items():
                        if 0 in frets and any(a <= tick < b for a, b in phrases):
                            frets[7] = frets.pop(0)
            tracks[(pid, di)] = t
    return 'mid', res, tempos, sigs, names, tracks


def build_notes(kind, res, tempo, track, ini):
    """the chords of one part on one difficulty: [(tick, frets, flags, len_ticks)]"""
    try:
        threshold = int(ini.get('hopo_frequency', 0))
    except ValueError:
        threshold = 0
    if threshold <= 0:
        eighth = str(ini.get('eighthnote_hopo', '')).strip().lower() in ('1', 'true')
        threshold = res // (2 if eighth else 3)
        if kind == 'chart':
            threshold += res // 192
    try:
        cutoff = int(ini.get('sustain_cutoff_threshold', -1))
    except ValueError:
        cutoff = -1
    if cutoff < 0:
        cutoff = res // 3 if kind == 'mid' else 0

    def inside(ranges, tick):
        return any(a <= tick < b for a, b in ranges)

    forced_ticks = set(track.forced) if kind == 'chart' else None
    out = []
    prev = None
    for tick in sorted(track.notes):
        frets = track.notes[tick]
        mask = 0
        for f in frets:
            mask |= OPEN if f == 7 else 1 << f
        if mask & OPEN and mask != OPEN:
            mask &= ~OPEN           # an open note charted with frets: keep the frets
        length = max(frets.values())
        if length <= cutoff:
            length = 0
        single = bin(mask).count('1') == 1
        natural = False
        if prev is not None and single and tick - prev[0] <= threshold and mask != prev[1]:
            natural = not (bin(prev[1]).count('1') > 1 and mask & prev[1])
        if kind == 'chart':
            hopo = natural != (tick in forced_ticks)
        else:
            hopo = natural
            for a, b, on in track.forced:
                if a <= tick < b:
                    hopo = on
        flags = 0
        if inside(track.taps, tick) and not mask & OPEN:
            flags |= F_TAP
        elif hopo:
            flags |= F_HOPO
        if inside(track.sp, tick):
            flags |= F_SP
        out.append([tick, mask, flags, length])
        prev = (tick, mask)
    # the last note of each Star Power phrase completes it
    for a, b in track.sp:
        last = None
        for n in out:
            if a <= n[0] < b:
                last = n
        if last is not None:
            last[2] |= F_SP_END
    # sustains stop short of the next note on the same frets (as Clone Hero trims them)
    for i, n in enumerate(out):
        if n[3]:
            for m in out[i + 1:]:
                if m[1] & n[1] or m[1] == OPEN or n[1] == OPEN:
                    gap = m[0] - n[0]
                    if n[3] > gap - res // 16:
                        n[3] = max(0, gap - res // 16)
                    break
    return out


def beat_lines(res, sigs, tempo, end_tick):
    sigs = sorted(sigs) or [(0, 4, 4)]
    if sigs[0][0] != 0:
        sigs.insert(0, (0, 4, 4))
    lines = []
    for i, (tick, num, den) in enumerate(sigs):
        stop = sigs[i + 1][0] if i + 1 < len(sigs) else end_tick
        step = max(1, res * 4 // max(1, den))
        n = 0
        t = tick
        while t < stop:
            lines.append((t, 0 if n % max(1, num) == 0 else 1))
            n += 1
            t += step
    return lines


# ------------------------------------------------------------------------------------------------------------ audio

def probe_seconds(path):
    try:
        out = subprocess.run(['ffprobe', '-v', 'error', '-show_entries', 'format=duration', '-of', 'csv=p=0', path],
                             capture_output=True, text=True, timeout=60).stdout.strip()
        return float(out)
    except (ValueError, subprocess.TimeoutExpired):
        return 0.0


def encode(inputs, out, quality):
    """mix the inputs (summed, as the games play stems) into one 48 kHz stereo Vorbis file"""
    cmd = ['ffmpeg', '-v', 'error', '-y', '-nostdin']
    for p in inputs:
        cmd += ['-i', p]
    chains = ['[%d:a]aformat=sample_fmts=fltp:sample_rates=48000:channel_layouts=stereo[a%d]' % (i, i)
              for i in range(len(inputs))]
    if len(inputs) > 1:
        chains.append(''.join('[a%d]' % i for i in range(len(inputs))) +
                      'amix=inputs=%d:normalize=0:duration=longest[out]' % len(inputs))
    else:
        chains[0] = chains[0].replace('[a0]', '[out]')
    cmd += ['-filter_complex', ';'.join(chains), '-map', '[out]', '-map_metadata', '-1',
            '-c:a', 'libvorbis', '-q:a', str(quality), out]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError('ffmpeg: ' + r.stderr.strip()[-400:])


def stems_of(src):
    """stem name -> file name"""
    stems = {}
    for n in src.files:
        base, ext = os.path.splitext(n)
        if ext in AUDIO_EXT and base != 'preview':
            if base not in stems or AUDIO_EXT.index(ext) < AUDIO_EXT.index(os.path.splitext(stems[base])[1]):
                stems[base] = n
    return stems


# ------------------------------------------------------------------------------------------------------------- pack

TAG = re.compile(r'<[^>]*>')


def clean(s):
    return TAG.sub('', s or '').strip()


def safe_name(s):
    s = re.sub(r'[\\/:*?"<>|\x00-\x1f]', '', s).strip(' .')
    return s[:80] or 'song'


def ms(v, default=-1):
    try:
        return int(float(v))
    except (TypeError, ValueError):
        return default


# bump when the chart output changes: songs packed before get new charts (their audio is kept)
CHART_VERSION = 2


def pack(path, out_root, force=False, quality=3, stem_quality=2):
    src = Source(path)
    ini = src.ini()
    if src.has('notes.mid'):
        kind, res, tempos, sigs, names, tracks = parse_mid(src.read('notes.mid'), ini)
    elif src.has('notes.chart'):
        raw = src.read('notes.chart')
        text = raw.decode('utf-8-sig', 'replace') if not raw.startswith(b'\xff\xfe') else raw.decode('utf-16')
        kind, res, tempos, sigs, names, tracks = parse_chart(text)
        if not ini:
            ini = {}
    else:
        raise ValueError('no chart')
    if not tracks:
        raise ValueError('no 5-fret parts')
    name = clean(ini.get('name')) or os.path.basename(path.rstrip('/'))
    artist = clean(ini.get('artist')) or 'Unknown Artist'
    folder = os.path.join(out_root, safe_name('%s - %s' % (artist, name)))
    stamp = src.stamp()
    stamp_file = os.path.join(folder, '.source')
    old = open(stamp_file).read().split('\n') if os.path.exists(stamp_file) else []
    same_source = not force and old[:1] == [stamp]
    if same_source and len(old) > 2 and old[2] == 'chart %d' % CHART_VERSION:
        return folder, 'skipped'

    tempo = Tempo(res, tempos)
    delay = ms(ini.get('delay'), 0) / 1000.0

    def us(tick):
        return int(round((tempo.seconds(tick) + delay) * 1e6))

    stems = stems_of(src)
    if not stems:
        raise ValueError('no audio')
    tmp = tempfile.mkdtemp(prefix='ghvita-')
    try:
        audio_len = max(probe_seconds(src.disk_path(n, tmp)) for n in stems.values())
        # the parts and which stem each one mutes
        parts = sorted(set(pid for pid, _ in tracks))
        part_stem = {}
        for pid in parts:
            stem = PARTS[pid][3]
            if pid == 1 and 'bass' not in stems and 'rhythm' in stems and 2 not in parts:
                stem = 'rhythm'         # Clone Hero songs put the bass on rhythm.ogg when there's no rhythm part
            if stem in stems:
                part_stem[pid] = stem
        own = sorted(set(part_stem.values()))
        backing = [n for b, n in sorted(stems.items()) if b not in own]
        os.makedirs(folder, exist_ok=True)
        out_name = lambda stem: 'bass' if stem == 'rhythm' and 2 not in parts else stem
        out_stems = (['backing'] if backing else []) + [out_name(stem) for stem in own]
        # the same source packed before by an older chart version: only the chart is redone
        reuse = same_source and all(os.path.exists(os.path.join(folder, n + '.ogg')) for n in out_stems)
        if not reuse:
            for f in os.listdir(folder):
                if f.endswith('.ogg'):
                    os.remove(os.path.join(folder, f))
            if backing:
                encode([src.disk_path(n, tmp) for n in backing], os.path.join(folder, 'backing.ogg'), quality)
        for stem in own:
            out = out_name(stem)
            if not reuse:
                encode([src.disk_path(stems[stem], tmp)], os.path.join(folder, out + '.ogg'), stem_quality)
            for pid, s in part_stem.items():
                if s == stem:
                    part_stem[pid] = out
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # the chart
    end_tick = 0
    built = {}
    for key, t in sorted(tracks.items()):
        notes = build_notes(kind, res, tempo, t, ini)
        if key[0] == 5:
            # drums: as songconv.c, pads on the first lanes, the kick is the open bar, every hit struck
            for n in notes:
                n[1] = (n[1] >> 1 & 15) | (OPEN if n[1] & 1 else 0)
                n[2] &= ~(F_HOPO | F_TAP)
                n[3] = 0
        if notes:
            built[key] = notes
            end_tick = max(end_tick, notes[-1][0] + notes[-1][3])
    # beat lines run to the end of the audio
    last_tick = end_tick + res * 8
    while tempo.seconds(last_tick) < audio_len + 2 and last_tick < end_tick + res * 4000:
        last_tick += res * 4
    beats = beat_lines(res, sigs, tempo, last_tick)
    blob = io.BytesIO()
    w = blob.write
    tempo_pts = tempo.points
    w(b'VHC1')
    w(struct.pack('<IIIIII', 1, len(tempo_pts), len(beats), len(names), len(built), int(audio_len * 1000)))
    for tick, sec, uspq in tempo_pts:
        w(struct.pack('<iI', us(tick), uspq))
    for tick, k in beats:
        w(struct.pack('<iB3x', us(tick), k))
    for tick, label in sorted(names):
        label = label.replace('_', ' ').strip()
        w(struct.pack('<i60s', us(tick), label.encode('utf-8')[:59]))
    for (pid, di), notes in sorted(built.items()):
        t = tracks[(pid, di)]
        # phrases and solos this chart has notes in (charts often share one set of phrases between instruments)
        has = lambda a, b: any(a <= n[0] < b for n in notes)
        sp = sorted(r for r in t.sp if has(*r))
        solos = sorted(r for r in t.solos if has(*r))
        w(struct.pack('<BBHIII', pid, di, 0, len(notes), len(sp), len(solos)))
        for tick, mask, flags, length in notes:
            start = us(tick)
            ln = us(tick + length) - start if length else 0
            w(struct.pack('<iIBBH', start, ln, mask, flags, min(65535, int(round(length * 100 / res)))))
        for a, b in sp:
            w(struct.pack('<ii', us(a), us(b)))
        for a, b in solos:
            w(struct.pack('<ii', us(a), us(b)))
    with open(os.path.join(folder, 'notes.vhc'), 'wb') as f:
        f.write(blob.getvalue())

    # the cover
    art = [n for n in ('album.png', 'album.jpg', 'album.jpeg') if src.has(n)]
    if art:
        try:
            from PIL import Image
            im = Image.open(io.BytesIO(src.read(art[0]))).convert('RGB')
            im = im.resize((256, 256), Image.LANCZOS)
            im.save(os.path.join(folder, 'album.jpg'), quality=88)
        except Exception as e:
            print('  cover: %s' % e, file=sys.stderr)

    # the metadata
    def diff(k):
        v = ms(ini.get(k), -1)
        return v
    preview = ms(ini.get('preview_start_time'), -1)
    if preview <= 0:
        preview = int(audio_len * 300)        # 30% in
    lines = ['[song]',
             'name = ' + name,
             'artist = ' + artist,
             'album = ' + clean(ini.get('album')),
             'genre = ' + clean(ini.get('genre')),
             'year = ' + clean(ini.get('year')).lstrip(', '),
             'charter = ' + clean(ini.get('charter') or ini.get('frets')),
             'song_length = %d' % int(audio_len * 1000),
             'preview_start_time = %d' % preview,
             'parts = ' + ','.join(PARTS[p][3] if p != 3 else 'coop' for p in sorted(set(k[0] for k in built))),
             'stems = ' + ','.join(out_stems)]
    for pid, stem in sorted(part_stem.items()):
        lines.append('stem_%d = %s' % (pid, stem))
    for k in ('diff_guitar', 'diff_bass', 'diff_rhythm', 'diff_guitar_coop', 'diff_keys', 'diff_drums'):
        if diff(k) >= 0:
            lines.append('%s = %d' % (k, diff(k)))
    with open(os.path.join(folder, 'song.ini'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    with open(stamp_file, 'w') as f:
        f.write('%s\n%s\nchart %d\n' % (stamp, os.path.abspath(path), CHART_VERSION))
    return folder, 'charts redone' if reuse else 'packed'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('sources', nargs='+')
    ap.add_argument('-o', '--out', default='songs')
    ap.add_argument('-j', '--jobs', type=int, default=max(1, (os.cpu_count() or 2) // 2))
    ap.add_argument('--force', action='store_true')
    ap.add_argument('-q', '--quality', type=int, default=3, help='Vorbis quality of the backing track (0-10)')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    srcs = list(find_sources(a.sources))
    print('%d songs' % len(srcs))
    ok = bad = 0
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        futs = {ex.submit(pack, s, a.out, a.force, a.quality): s for s in srcs}
        for fut in concurrent.futures.as_completed(futs):
            s = futs[fut]
            try:
                folder, what = fut.result()
                ok += 1
                print('%s: %s' % (what, os.path.basename(folder)))
            except Exception as e:
                bad += 1
                print('FAILED %s: %s' % (s, e))
    print('%d packed, %d failed' % (ok, bad))
    return 0 if bad == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
