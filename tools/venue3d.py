#!/usr/bin/env python3
"""World Tour's venues as models the game draws behind the 3D band's close shots (assets/venue3d/<zone>.v3d):
the stage and what stands round it, thinned with Blender, its textures in one atlas (the ones that tile kept apart),
turned to the stage's own axes (x to the audience's right, y up from the singer's floor, z toward the audience, the
origin under the drummer) with the band's marks.
  tools/venue3d.py <textures dir> <work dir> [zone ...]      (~/.cache/ghv-venv/bin/python; Blender as tools/band.py)
File: "V3D1", parts, 4 marks x 3 floats (drummer, vocalist, guitarist, bassist); each part: texture name[32], vertex
and triangle counts, wraps; vertices (3 floats place, 2 floats texture place, light u8 + 3 pad); u16 indices."""
import glob, importlib.util, json, math, os, struct, subprocess, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('band', os.path.join(HERE, 'band.py'))
band = importlib.util.module_from_spec(spec)
spec.loader.exec_module(band)
wc = band.wc
spec = importlib.util.spec_from_file_location('venues', os.path.join(HERE, 'venues.py'))
venues = importlib.util.module_from_spec(spec)
spec.loader.exec_module(venues)        # (its lists of what the export draws wrong, shared with the venue pictures)
OUT = os.path.join(HERE, '..', 'assets', 'venue3d')
NEAR_M, MID_M = 16.0, 70.0              # metres from the stage: what's near keeps its shape, the skyline hardly any
BUDGET = {'near': 9500, 'mid': 4000, 'far': 3200}
THIN_KEEP = 2600                        # triangles of trusses and rails kept (after their light thinning)
BEHIND = 11.0                           # nothing's seen this far out toward the audience: the camera's nearer
ROLES = ['drummer', 'vocalist', 'guitarist', 'bassist']


def texture_kind(path, cache={}):
    # opaque, cutout (alpha either side of the test), or soft (haze, beams: not for the depth-tested stage)
    if path not in cache:
        px = np.asarray(Image.open(path).convert('RGBA').resize((32, 32))).astype(np.float32)
        rgb = px[:, :, :3]
        sat = (rgb.max(2) - rgb.min(2)).mean()
        if rgb.reshape(-1, 3).std(0).mean() < 26 and sat > 110:
            cache[path] = 'soft'        # one flat vivid colour: a video screen's stand-in. Left out (a screen that's off)
            return cache[path]
        a = px[:, :, 3] / 255
        mid = ((a > 0.15) & (a < 0.85)).mean()
        # smoke and fog cards: grey or white with soft edges. Drawn as cut-outs they stood in the shot as flat lavender
        # sheets with stepped edges
        a64 = np.asarray(Image.open(path).convert('RGBA').resize((64, 64))).astype(np.float32)
        aa = a64[:, :, 3] / 255
        some = (aa > 0.3)
        grey = some.any() and float((a64[:, :, :3].max(2) - a64[:, :, :3].min(2))[some].mean()) < 12
        fuzzy = ((aa > 0.05) & (aa < 0.95)).mean() / max(float((aa > 0.05).mean()), 1e-6)
        if grey and fuzzy > 0.3 and (aa < 0.95).mean() > 0.2:
            cache[path] = 'soft'
            return cache[path]
        # plain white or near-white grey with no colour: a texture the game tints with a material colour (that colour
        # isn't in the export), or a light shaft's streaks. As they are they stood in the shot as bright white sheets
        rgb64 = a64[:, :, :3]
        lum, chroma = float(rgb64.mean()), float((rgb64.max(2) - rgb64.min(2)).mean())
        if chroma < 5 and lum > 190:
            cache[path] = 'soft'
            return cache[path]
        cache[path] = 'opaque' if (a > 0.85).mean() > 0.97 else 'soft' if mid > 0.3 or (a > 0.5).mean() < 0.12 else 'cutout'
    return cache[path]


def flat_grey(path, cache={}):
    # one flat grey all over, nothing in it: a video screen's stand-in (Nashville's riser front, mid-grey slabs)
    if path not in cache:
        a = np.asarray(Image.open(path).convert('RGBA').resize((32, 32))).astype(np.float32)
        v = a[:, :, :3].mean(2)
        sat = (a[:, :, :3].max(2) - a[:, :, :3].min(2)).mean()
        cache[path] = bool(v.std() < 4 and sat < 4 and 50 < v.mean() < 210 and (a[:, :, 3] < 235).mean() < 0.05)
    return cache[path]


def bake(zone, tex_dir, work):
    name = 'venue_ghwt_' + zone
    bones, meshes, clips = wc.read(os.path.join(tex_dir, name + '.wtch'))
    j = json.load(open(os.path.join(tex_dir, name + '.json')))
    blends = json.load(open(os.path.join(tex_dir, name + '.parts.json')))['blend']
    more_hide, show, solid, hide_words, tint = venues.zone_fixes(zone)
    hide = (set(j.get('hide', [])) | more_hide) - show
    f = np.array(j.get('facing', [0, 0, -1]), float) * [1, 1, -1]
    f[1] = 0
    f /= np.linalg.norm(f)
    marks = {r: np.array(j['marks'][r], float) * [1, 1, -1] for r in ROLES if r in j.get('marks', {})}
    if 'drummer' not in marks:
        print(zone, 'no marks')
        return
    origin = marks['drummer'].copy()
    origin[1] = marks.get('vocalist', marks['drummer'])[1]

    def to_stage(P):
        d = np.asarray(P, float) - origin
        return np.stack([d[:, 0] * f[2] - d[:, 2] * f[0], d[:, 1], d[:, 0] * f[0] + d[:, 2] * f[2]], 1)

    keep = []
    for i, (tex, V, T) in enumerate(meshes):
        if i in hide or (i not in solid and i < len(blends) and blends[i] != 'opaque') or not tex or len(T) == 0:
            continue
        if any(w in tex for w in hide_words):
            continue
        src = band.find_tex(tex_dir, tex)
        if not src or (i not in solid and texture_kind(src) == 'soft'):
            continue
        # the brightness it's lit at, carried through the thinning in the vertex alpha (the model doesn't keep that):
        # a tinted part's, a flat grey screen's as one that's off
        shade = tint.get(i, 0.3 if flat_grey(src) else 1.0)
        P = to_stage(np.asarray(V['p'], float) * [1, 1, -1])    # Unity's left-handed space back to GHWT's, as the marks are
        T = np.asarray(T)
        # triangles wholly out behind where any camera stands, and the ones below the world: gone
        ok = ~((P[T][:, :, 2] > BEHIND).all(1))
        T = T[ok]
        if len(T) == 0:
            continue
        c = P[np.unique(T)].mean(0)
        dist = math.hypot(c[0], c[2] - 1.5)
        # the file's texture places count up from the picture's bottom row, the game's down from its top
        keep.append((tex, P, np.asarray(V['uv'], np.float32) * [1, -1] + [0, 1], np.full(len(P), shade, np.float32), T, dist))
    # thin things (a lighting truss's tubes, railings: thousands of slivers) fall to shards when thinned: the nearest of
    # them stay, lightly thinned, up to THIN_KEEP triangles; the rest go
    thin_ix = []
    for i, (tex, P, uv, a, T, dist) in enumerate(keep):
        if len(T) < 200:
            continue
        e1, e2 = P[T[:, 1]] - P[T[:, 0]], P[T[:, 2]] - P[T[:, 0]]
        area = np.linalg.norm(np.cross(e1, e2), axis=1) / 2
        edge = np.maximum(np.linalg.norm(e1, axis=1), np.linalg.norm(e2, axis=1))
        # slivers: long for their area, or tiny
        if np.median(area / np.maximum(edge * edge, 1e-9)) < 0.06 or np.median(area) < 0.004:
            thin_ix.append(i)
    thin_ix.sort(key=lambda i: keep[i][5])
    budget, thin_old, drop = THIN_KEEP, {}, set()
    for i in thin_ix:
        cost = len(keep[i][4]) * 0.45
        if cost <= budget:
            budget -= cost
            thin_old[i] = 0.45
        else:
            drop.add(i)
    thin_ratio, kept = {}, []
    for i, k in enumerate(keep):
        if i in drop:
            continue
        if i in thin_old:
            thin_ratio[len(kept)] = thin_old[i]
        kept.append(k)
    keep = kept
    total = {k: 0 for k in BUDGET}
    cls = []
    for tex, P, uv, a, T, dist in keep:
        k = 'near' if dist < NEAR_M else 'mid' if dist < MID_M else 'far'
        cls.append(k)
        if len(T) > 40 and len(cls) - 1 not in thin_ratio:
            total[k] += len(T)
    ratio = {k: min(1.0, BUDGET[k] / max(total[k], 1)) for k in BUDGET}
    arrs = {'count': len(keep)}
    for i, (tex, P, uv, a, T, dist) in enumerate(keep):
        used = np.unique(T)
        remap = np.full(len(P), -1)
        remap[used] = np.arange(len(used))
        n = len(used)
        arrs.update({'p%d' % i: P[used].astype(np.float32), 'uv%d' % i: uv[used], 'j%d' % i: np.zeros((n, 4), np.int32),
                     'w%d' % i: np.tile(np.array([1, 0, 0, 0], np.float32), (n, 1)), 'a%d' % i: a[used],
                     't%d' % i: remap[T].astype(np.int32), 'r%d' % i: 1.0 if len(T) <= 40 else thin_ratio[i] if i in thin_ratio else max(ratio[cls[i]], 0.07)})
    src, dst = os.path.join(work, zone + '_in.npz'), os.path.join(work, zone + '_out.npz')
    np.savez(src, **arrs)
    subprocess.run([band.BLENDER, '-b', '--python', os.path.join(HERE, 'band_decimate_bl.py'), '--', src, dst], check=True,
                   stdout=subprocess.DEVNULL)
    d = np.load(dst)
    parts = []
    for i, (tex, P, uv, a, T, dist) in enumerate(keep):
        if len(d['t%d' % i]):
            parts.append((tex, {k: d['%s%d' % (k, i)] for k in ('p', 'n', 'uv', 'j', 'w', 'a')}, d['t%d' % i], False))
    os.remove(src)
    os.remove(dst)
    os.makedirs(os.path.join(OUT, 'tex'), exist_ok=True)
    packed = band.pack_atlas(parts, tex_dir, zone, OUT, 1024, 0.16, '_atlas', 3, 128)
    # the parts that kept a texture of their own: one part a texture
    merged, order = {}, []
    for tname, V, T, wraps in packed:
        if tname not in merged:
            merged[tname] = [[], [], [], 0, wraps, []]
            order.append(tname)
        m = merged[tname]
        m[0].append(V['p'])
        m[1].append(V['uv'])
        m[2].append(np.asarray(T) + m[3])
        m[3] += len(V['p'])
        m[5].append(np.asarray(V['a'], np.float32).reshape(-1))
    recs = []
    for tname in order:
        P, uv, T, n, wraps, A = merged[tname]
        P, uv, T, A = np.concatenate(P), np.concatenate(uv), np.concatenate(T), np.clip(np.concatenate(A), 0, 1)
        # 65535 vertices a part: split what's bigger
        for s in range(0, len(T), 20000):
            tt = T[s:s + 20000]
            used = np.unique(tt)
            remap = np.full(len(P), -1)
            remap[used] = np.arange(len(used))
            rec = np.zeros(len(used), np.dtype([('p', '<f4', 3), ('uv', '<f4', 2), ('l', 'u1', 4)]))
            rec['p'] = P[used]
            rec['uv'] = uv[used]
            # the show lights the stage: what's further from it is darker
            dist = np.hypot(P[used][:, 0], P[used][:, 2] - 1.5)
            rec['l'][:, 0] = np.clip((1.12 - dist / 60) * 255, 70, 255) * A[used]
            recs.append((tname, rec, remap[tt].astype('<u2'), wraps))
    with open(os.path.join(OUT, zone + '.v3d'), 'wb') as o:
        o.write(b'V3D1' + struct.pack('<i', len(recs)))
        for r in ROLES:
            m = to_stage(marks.get(r, marks['drummer'])[None])[0]
            o.write(struct.pack('<3f', *m))
        for tname, rec, T, wraps in recs:
            o.write(tname.encode().ljust(32, b'\0'))
            o.write(struct.pack('<iii', len(rec), len(T), 1 if wraps else 0))
            o.write(rec.tobytes())
            o.write(T.tobytes())
    # the atlas in 256 colours where nothing in it is see-through at the edges: a third of the download
    print(zone, sum(len(r) for _, r, _, _ in recs), 'verts', sum(len(t) for _, _, t, _ in recs), 'tris (from',
          sum(len(m[2]) for m in meshes), ')', len(recs), 'parts', os.path.getsize(os.path.join(OUT, zone + '.v3d')) // 1024, 'KB')


def main():
    tex_dir, work = sys.argv[1], sys.argv[2]
    os.makedirs(work, exist_ok=True)
    zones = sys.argv[3:] or sorted(os.path.basename(p)[11:-5] for p in glob.glob(os.path.join(tex_dir, 'venue_ghwt_*.wtch')))
    for z in zones:
        bake(z, tex_dir, work)
    # every texture in 256 colours (alpha kept): a third of the download, and nothing a stage in the dark shows
    for f in glob.glob(os.path.join(OUT, 'tex', '*.png')):
        im = Image.open(f)
        if im.mode != 'P':
            im.convert('RGBA').quantize(256, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE).save(f, optimize=True)
    # a venue's textures in one file beside its model (<zone>.v3t: "V3T1", count, then name[32] + size each, then the
    # PNGs): some 770 loose pictures took an update past the 1024 files the Updater app moves, and it stopped short
    for f in sorted(glob.glob(os.path.join(OUT, '*.v3d'))):
        if os.path.basename(f)[:-4] not in zones:
            continue                    # (only the zones baked now: the others' textures aren't here to pack)
        d = open(f, 'rb').read()
        n, at, names = struct.unpack_from('<i', d, 4)[0], 56, []
        for _ in range(n):
            name = d[at:at + 32].split(b'\0')[0].decode()
            vc, tc, _w = struct.unpack_from('<3i', d, at + 32)
            at += 44 + vc * 24 + tc * 6
            if name and name not in names and os.path.exists(os.path.join(OUT, 'tex', name + '.png')):
                names.append(name)
        blobs = [open(os.path.join(OUT, 'tex', x + '.png'), 'rb').read() for x in names]
        with open(f[:-4] + '.v3t', 'wb') as o:
            o.write(b'V3T1' + struct.pack('<i', len(names)))
            for x, b in zip(names, blobs):
                o.write(x.encode().ljust(32, b'\0') + struct.pack('<I', len(b)))
            for b in blobs:
                o.write(b)
    import shutil
    shutil.rmtree(os.path.join(OUT, 'tex'), ignore_errors=True)


if __name__ == '__main__':
    main()
