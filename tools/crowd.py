#!/usr/bin/env python3
# GHWT's crowd (crowd_ped_01..04, crowd_pedf_1..2 from YargLook) as sprite sheets for GH Vita, seen from behind as the
# camera in the audience sees them. Each ped has World Tour's crowd moods as clips: bad (bored, arms down), reg (nodding
# along), good (cheering), sp (Star Power: going wild). For each mood the short stretch of the clip that loops best is
# posed over FRAMES frames, filmed by tools/band_render.c and packed into assets/crowd/<ped>.png: FRAMES columns x one
# row per mood (MOODS order), cells CW x CH at PPM pixels per metre. game.c draw_crowd uses the same numbers.
# usage: tools/crowd.py <textures dir> <work dir>    (needs a GL display: DISPLAY=:77)
import importlib.util, math, os, struct, subprocess, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'assets', 'crowd')
RENDER = os.path.join(HERE, '..', 'build-tools', 'band_render')
spec = importlib.util.spec_from_file_location('wc', os.path.expanduser('~/mods/refs/yarg/ghwt-tools/wtch_check.py'))
wc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wc)

PEDS = ['crowd_ped_01', 'crowd_ped_02', 'crowd_ped_03', 'crowd_ped_04', 'crowd_pedf_1', 'crowd_pedf_2']
MOODS = ['bad', 'reg_m', 'good_m', 'sp_m']
FRAMES = 8
CW, CH, PPM, Y0 = 52, 100, 46, -0.05
SS = 4
PITCH = math.radians(8)         # the camera's a little above their heads


def loop_window(clip, lo=14, hi=36):
    # the stretch of the clip that closes on itself best: lo..hi frames (half a second to over a second at 30 fps)
    _, fps, nf, animp, Q, P = clip
    best, score = (0, hi), 1e18
    for span in range(lo, min(hi, nf - 1) + 1):
        for s in range(0, nf - span, 2):
            d = float(np.abs(Q[s] - Q[s + span]).mean())
            # it must move: a still stretch closes perfectly and says nothing
            mid = float(np.abs(Q[s] - Q[s + span // 2]).mean())
            d = d / (mid + 0.02)
            if d < score:
                best, score = (s, span), d
    return best


def skinned(meshes, skin):
    out = []
    S = np.stack(skin)
    for tex, V, T in meshes:
        P = np.c_[V['p'], np.ones(len(V))]
        N = np.c_[V['n'], np.zeros(len(V))]
        p = np.zeros((len(V), 4))
        n = np.zeros((len(V), 4))
        for k in range(4):
            M = S[V['j'][:, k]]
            w = V['w'][:, k:k + 1]
            p += w * np.einsum('nij,nj->ni', M, P)
            n += w * np.einsum('nij,nj->ni', M, N)
        out.append((p[:, :3], n[:, :3]))
    return out


def bake(tex_dir, work, ped):
    bones, meshes, clips = wc.read(os.path.join(tex_dir, ped + '.wtch'))
    byname = {c[0]: c for c in clips}
    frames = []
    for mood in MOODS:
        clip = byname[mood]
        s, span = loop_window(clip)
        for i in range(FRAMES):
            M = wc.pose(bones, clip, s + (i * span) // FRAMES)
            frames.append(skinned(meshes, [m @ b[4] for m, b in zip(M, bones)]))
        print(ped, mood, 'from', s, 'span', span)
    binp = os.path.join(work, ped + '.bin')
    ca, sa = math.cos(PITCH), math.sin(PITCH)
    with open(binp, 'wb') as o:
        o.write(struct.pack('<ii', len(meshes), len(frames)))
        for tex, V, T in meshes:
            o.write(tex.encode()[:32].ljust(32, b'\0'))
            o.write(struct.pack('<ii', len(V), len(T)))
            o.write(np.asarray(V['uv'], '<f4').tobytes())
            o.write(np.asarray(T, '<u4').tobytes())
        for fr in frames:
            for p, n in fr:
                # turned round: their backs to the camera, facing the stage
                x, y, z = -p[:, 0], p[:, 1], -p[:, 2]
                nx, ny, nz = -n[:, 0], n[:, 1], -n[:, 2]
                ln = np.sqrt(nx * nx + ny * ny + nz * nz) + 1e-9
                a = np.stack([x, y * ca + z * sa, -(z * ca - y * sa), nx / ln, ny / ln, -nz / ln], 1)
                o.write(np.asarray(a, '<f4').tobytes())
    pre = os.path.join(work, ped)
    subprocess.run([RENDER, tex_dir, binp, pre, str(CW * SS), str(CH * SS), str(PPM * SS), str(Y0)], check=True)
    os.remove(binp)
    sheet = Image.new('RGBA', (CW * FRAMES, CH * len(MOODS)), (0, 0, 0, 0))
    for i in range(len(frames)):
        im = Image.open('%s_%02d.png' % (pre, i)).convert('RGBa').resize((CW, CH), Image.BOX).convert('RGBA')
        sheet.paste(im, ((i % FRAMES) * CW, (i // FRAMES) * CH))
    out = os.path.join(OUT, ped + '.png')
    sheet.quantize(256, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE).save(out, optimize=True)
    print(out, os.path.getsize(out) // 1024, 'KB')


def main():
    tex_dir, work = sys.argv[1], sys.argv[2]
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(work, exist_ok=True)
    for ped in PEDS:
        bake(tex_dir, work, ped)


main()
