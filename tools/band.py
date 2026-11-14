#!/usr/bin/env python3
# GHWT band members as sprite sheets for GH Vita. Each member's 100 bpm loop (band_<role>[_N].wtch from YargLook) is
# posed with the offline skin + IK of ghwt-tools/wtch_check.py over the 4 beats that loop best (the last beat is blended
# back into the first, so there's no jump at the wrap), filmed by
# tools/band_render.c from the audience (15 degrees down) and packed into assets/band/<role>_<n>.png: 8x3 cells
# (standing 170x256 at 120 px/m, the drummer and kit 266x232 at 88 px/m: about the size they're drawn), 256-colour with alpha so a sheet loads fast.
# band.txt lists the sheets; game.c's band_cell has the same numbers.
# usage: tools/band.py <textures dir> <work dir> [member ...]    (needs a GL display: DISPLAY=:77)
import glob, importlib.util, math, os, struct, subprocess, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'assets', 'band')
RENDER = os.path.join(HERE, '..', 'build-tools', 'band_render')
spec = importlib.util.spec_from_file_location('wc', os.path.expanduser('~/mods/refs/yarg/ghwt-tools/wtch_check.py'))
wc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wc)

FRAMES, COLS, ROWS = 48, 8, 6
SING = 32               # the singer's sheet: this many frames of the loop with the mic at the mouth, the rest with it down
CELLS = {'drummer': (266, 232, 88, -0.62)}
STAND = (170, 256, 120, -0.08)      # cell w, h, pixels per metre, the cell's bottom in metres
SS = 4                  # supersampling of the render
BEATS = 4
BLEND = 18             # clip frames: one beat
PITCH = math.radians(15)


def pick_clip(clips, role):
    want = 't100' if role != 'drummer' else 'hs_100'
    for c in clips:
        if c[0] == want:
            return c
    return clips[0]


def loop_start(bones, clip, nb_ids, span, role):
    # the window of 4 beats that closes best and stays near one spot, starting on a beat (the loops are 100 bpm from
    # frame 0), far enough in for the blend into the wrap. The singer's has the mic in hand: BandAnimator's docked test
    _, fps, nf, animp, Q, P = clip
    best, score = None, 1e18
    ai = np.cumsum(animp) - 1
    pel = ai[nb_ids['bone_pelvis']] if animp[nb_ids['bone_pelvis']] else None
    beat = int(round(60 / 100 * fps))
    mic = nb_ids.get('Bone_Mic_Microphone')
    for s in range(BLEND, nf - span - 1, beat):
        d = float(np.abs(Q[s] - Q[s + span]).mean())
        if role == 'vocalist' and mic is not None:
            W = [wc.pose(bones, clip, s + k * span // 4) for k in range(4)]
            d += 2.0 * sum(np.linalg.norm(m[mic][:3, 3] - m[bones[mic][1]][:3, 3]) < 0.15 for m in W)
        if pel is not None:
            seg = P[s:s + span + 1, pel, :2]
            d += 2.0 * float(np.linalg.norm(seg[0] - seg[-1])) + 0.6 * float(np.ptp(seg, axis=0).max())
        if d < score:
            best, score = s, d
    return best


def blended(clip, f, end):
    # one frame of the clip, easing over the last beat into what came a loop earlier: it lands exactly on the first frame
    name, fps, nf, animp, Q, P = clip
    span = end[1] - end[0]
    w = max(0.0, (f - (end[1] - BLEND)) / BLEND)
    w = w * w * (3 - 2 * w)
    q, b = Q[f].copy(), Q[f - span]
    q[(q * b).sum(1) < 0] *= -1
    return (name, fps, 1, animp, (q * (1 - w) + b * w)[None], (P[f] * (1 - w) + P[f - span] * w)[None])


def hold_mic(rig, sing, rest_up=np.array([0, 1.0, 0])):
    # BandAnimator.Solve for the singer: the mic is in the right hand; a left hand out of reach hangs at the side
    mic = rig.p('Bone_Mic_Microphone')
    fwd = np.array([0, 0, -1.0])
    axis = None
    if sing:
        # singing: the grille (the mic's +z end) at the lips, the handle down and out in front of the chin
        axis = rest_up * 0.7 - fwd * 0.7
        axis /= np.linalg.norm(axis)
        centre = rig.p('Bone_Jaw') + fwd * 0.13 - axis * 0.1
        mic = centre - axis * 0.06
    for side in 'rl':
        sh, el, pm = rig.p('bone_bicep_' + side), rig.p('bone_forearm_' + side), rig.p('bone_palm_' + side)
        ln = np.linalg.norm(el - sh) + np.linalg.norm(pm - el)
        tgt = mic if side == 'r' else rig.p('bone_ik_hand_slave_l')
        out = np.array([1.0 if sh[0] > rig.p('bone_chest')[0] else -1.0, 0, 0])
        d = -rest_up * 0.95 + fwd * 0.12 + out * 0.07
        hang = sh + d / np.linalg.norm(d) * ln * 0.95
        # eased, as in BandAnimator (a hard switch snapped the arm between two frames)
        w = np.clip((1.12 - np.linalg.norm(tgt - sh) / ln) / 0.12, 0, 1)
        if side == 'l':
            tgt = hang + (tgt - hang) * (w * w * (3 - 2 * w))
        rig.two_bone('bone_bicep_' + side, 'bone_forearm_' + side, 'bone_palm_' + side, tgt,
                     el - fwd * 0.4 - rest_up * 0.3)
        if side == 'r':
            # off the stand the mic stays IN the hand, not where the loop left it (a mic out of reach floated)
            M = rig.W[rig.ix('Bone_Mic_Microphone')].copy()
            M[:3, 3] = rig.p('bone_palm_r')
            if sing:
                M[:3, :3] = rotation_between(M[:3, 2], axis) @ M[:3, :3]
                M[:3, 3] = centre
            rig.set_world(rig.ix('Bone_Mic_Microphone'), M)
            mic = rig.p('Bone_Mic_Microphone')


def load_hands(tex_dir, sex):
    # GHWT's partial hand animations (see YargLook's GhwtHands.cs): name -> (frames, {bone: frames x 7 (pos, quat)})
    d = open(os.path.join(tex_dir, 'ghwt_hands_%s.wthd' % sex), 'rb').read()
    n = struct.unpack_from('<i', d, 4)[0]
    o, out = 8, {}
    for _ in range(n):
        name = d[o:o + 48].split(b'\0')[0].decode()
        fps, frames, nb = struct.unpack_from('<fii', d, o + 48)
        o += 60
        tr = {}
        for b in range(nb):
            bone = d[o:o + 32].split(b'\0')[0].decode()
            o += 32
            tr[bone] = np.frombuffer(d, '<f4', frames * 7, o).reshape(frames, 7)
            o += frames * 28
        out[name] = (frames, tr)
    return out


def sex_of(tex_dir, member):
    for line in open(os.path.join(tex_dir, 'ghwt_band_sex.txt')):
        k = line.split()
        if len(k) == 2 and k[0] == 'band_' + member:
            return k[1]
    return 'm'


# the sheets are cells the chart picks (game.c band_cell), 6 frames of a stroke for each of 8 hand shapes: single notes
# green to orange, then three chords. GhwtHands.cs's shapes and neck positions
SHAPES = [('roll3_1', 5), ('roll3_2', 5), ('roll3_3', 5), ('roll3_4', 5), ('roll3_4', 6), ('bar1', 3), ('bar3', 5),
          ('bar2', 7)]
STROKE = 6
# guitars and bass: the body grooves through its loop (GROOVE steps over the 4 beats, as GH's band rocks), each step
# with 3 frames of a strum (ready, struck, through) on one hand shape; cell = step * 3 + strum (game.c band_cell)
GROOVE, STRUMS = 16, 3
STRUM_K = [0, 2, 4]
GROOVE_SHAPE = 1
# the drummer: 8 frames of a hit for each of 6 strikes, the stick swinging from the groove down onto the drum and back.
# Drums in the drummer's model space (BandAnimator.Pad*)
PAD_SNARE, PAD_TOM, PAD_FLOOR = (0.32, 0.735, -0.38), (0.20, 0.965, -0.63), (-0.24, 0.705, -0.38)
PAD_HAT, PAD_CRASH = (0.62, 1.015, -0.38), (-0.42, 1.05, -0.42)     # the crash: the cymbal on the right hand's side
# each hand keeps to its side of the kit: arms crossed to the hi-hat hide the face at this size
STRIKES = [{'r': PAD_SNARE}, {'l': PAD_HAT}, {'r': PAD_TOM}, {'r': PAD_CRASH}, {'r': PAD_FLOOR},
           {'r': PAD_SNARE, 'l': PAD_HAT}]
HIT = 8
HIT_W = [0, 0.55, 0.9, 1, 0.9, 0.6, 0.25, 0]        # how far from the groove
HIT_RAISE = [1, 1, 0.55, 0, 0.2, 0.5, 0.8, 1]       # the stick's cock
STICK_LEN = 0.382


def hand_pose(anims, bass, male, shape, k):
    # the chord, fret and strum of one cell: what HandPlayer drives from the chart
    chord, fret = SHAPES[shape]
    kind = 'male' if male else 'fmale'
    over = {}
    fname = ('gh_rocker_male_fret_bass_%02d_d' if bass else 'gh_rocker_male_fret_%02d_d') % fret
    fz = float(anims[fname][1]['bone_guitar_fret_pos'][0][2])
    over['bone_guitar_fret_pos'] = np.array([0, 0, fz, 0, 0, 0, 1.0])
    frames, tr = anims.get('gh3_hnd_guit_chord_%s_%s' % (kind, chord), anims['gh3_hnd_guit_chord_%s_empty' % kind])
    for b, x in tr.items():
        over[b] = x[0]
    sname = 'gh_rocker_%s_strum_%s_med_01' % (kind, 'bass' if bass else 'norm')
    frames, tr = anims[sname]
    n = int(k / (STROKE - 1) * (frames - 1))
    for b, x in tr.items():
        over[b] = x[n]
    return over


def rotation_between(a, b):
    a, b = a / np.linalg.norm(a), b / np.linalg.norm(b)
    v, c = np.cross(a, b), float(a @ b)
    K = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + K + K @ K / (1 + max(c, -0.999))


def swing_stick(rig, side, pad, w, raise_):
    # BandAnimator.DrumPlay: the stick out from the shoulder, tilted onto the head at impact, the wrist cocking it up
    names = {n.lower(): i for i, n in enumerate(rig.N)}
    stick = names['bone_guitar_body' if side == 'l' else 'bone_guitar_fret_pos']
    A, B, C = rig.p('bone_bicep_' + side), rig.p('bone_forearm_' + side), rig.p('bone_palm_' + side)
    M0 = rig.W[stick].copy()
    g0 = M0[:3, 3].copy()
    t0 = (M0 @ np.array([0, 0, STICK_LEN, 1]))[:3]
    length = np.linalg.norm(t0 - g0)
    reach = (np.linalg.norm(B - A) + np.linalg.norm(C - B)) * 0.97
    P = np.array(pad, float)
    h = P - A
    h[1] = 0
    h = h / max(np.linalg.norm(h), 1e-6)
    pitch = math.radians(-22 + 60 * raise_)
    d = h * math.cos(pitch) + np.array([0, math.sin(pitch), 0])
    tip = P + np.array([0, 0.06 * raise_, 0])
    g = tip - d / np.linalg.norm(d) * length
    sg = g - A
    if np.linalg.norm(sg) > reach:
        g = A + sg / np.linalg.norm(sg) * reach
    R_full = rotation_between(t0 - g0, tip - g)
    # part way: the same turn, smaller
    ang = math.acos(np.clip((np.trace(R_full) - 1) / 2, -1, 1))
    axis = np.array([R_full[2, 1] - R_full[1, 2], R_full[0, 2] - R_full[2, 0], R_full[1, 0] - R_full[0, 1]])
    R_part = np.eye(3)
    if np.linalg.norm(axis) > 1e-6:
        axis /= np.linalg.norm(axis)
        K = np.array([[0, -axis[2], axis[1]], [axis[2], 0, -axis[0]], [-axis[1], axis[0], 0]])
        a = ang * w
        R_part = np.eye(3) + math.sin(a) * K + (1 - math.cos(a)) * K @ K
    M = M0.copy()
    M[:3, :3] = R_part @ M0[:3, :3]
    M[:3, 3] = g0 + (g - g0) * w
    rig.set_world(stick, M)


def pose_over(bones, clip, over):
    # wc.pose of a one-frame clip, with the hand animations' bones replaced: rotations always, positions on the IK targets
    _, _, _, animp, Q, P = clip
    model = []
    ai = np.cumsum(animp) - 1
    for i, (name, par, pos, rot, bp) in enumerate(bones):
        p = P[0, ai[i]] if animp[i] else np.array(pos)
        q = Q[0, i]
        x = over.get(name.lower())
        if x is not None:
            q = x[3:7]
            if 'ik_hand' in name.lower() or 'fret_pos' in name.lower():
                p = x[:3]
        l = wc.trs(p, q)
        model.append(l if par < 0 else model[par] @ l)
    return model


# palm = target * these (BandAnimator.GtrFixL/R)
FIX_L = np.array([[0, 0, -1.0], [-1, 0, 0], [0, 1, 0]])
FIX_R = np.array([[0, 0, 1.0], [1, 0, 0], [0, 1, 0]])


def guitar_hands(rig):
    # BandAnimator.Solve: elbows a little down, each palm exactly on its target, rotation included
    for side, fix in (('r', FIX_R), ('l', FIX_L)):
        tgt = rig.ix('bone_ik_hand_guitar_' + side)
        el = rig.p('bone_forearm_' + side)
        rig.two_bone('bone_bicep_' + side, 'bone_forearm_' + side, 'bone_palm_' + side, rig.W[tgt][:3, 3],
                     el + np.array([0, -0.12, 0]))
        pm = rig.ix('bone_palm_' + side)
        M = rig.W[pm].copy()
        M[:3, :3] = rig.W[tgt][:3, :3] @ fix
        rig.set_world(pm, M)


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


def bake(path, work, tex_dir):
    name = os.path.basename(path)[5:-5]
    role = name.split('_')[0]
    bones, meshes, clips = wc.read(path)
    N = {b[0]: i for i, b in enumerate(bones)}
    rest = wc.rest_model(bones)
    clip = pick_clip(clips, role)
    span = int(round(BEATS * 60 / 100 * clip[1]))
    s0 = loop_start(bones, clip, N, span, role)
    M0 = wc.pose(bones, clip, s0)
    g0 = None
    if role in ('guitarist', 'bassist'):
        gi = N['bone_guitar_body']
        g0 = (np.linalg.inv(M0[N['bone_pelvis']]) @ M0[gi], np.linalg.inv(M0[N['bone_chest']]) @ M0[gi])
    anims = load_hands(tex_dir, sex_of(tex_dir, name)) if g0 is not None else None
    frames, centre = [], []
    for i in range(FRAMES):
        f = s0 + i * span // FRAMES
        if role == 'vocalist':
            f = s0 + i * span // SING if i < SING else s0 + (i - SING) * span // (FRAMES - SING)
        if g0 is not None:
            f = s0 + (i // STRUMS) * span // GROOVE
        # the drummer keeps one pose (the chart moves the sticks); the guitars groove through the loop
        pose = blended(clip, f if role != 'drummer' else s0 + span - BLEND, (s0, s0 + span))
        if g0 is not None:
            rig = wc.Rig(bones, pose_over(bones, pose, hand_pose(anims, role == 'bassist', sex_of(tex_dir, name) != 'f',
                                                                 GROOVE_SHAPE, STRUM_K[i % STRUMS])))
        else:
            rig = wc.Rig(bones, wc.pose(bones, pose, 0))
        if role == 'drummer':
            for side, pad in STRIKES[i // HIT].items():
                swing_stick(rig, side, pad, HIT_W[i % HIT], HIT_RAISE[i % HIT])
        if role != 'drummer' or 'bone_ik_hand_slave_l' in N:
            wc.solve(rig, rest, role, (f - s0) / span * BEATS, g0)
        if role == 'vocalist':
            hold_mic(rig, i < SING)
        if g0 is not None:
            guitar_hands(rig)
        if role == 'drummer':
            # GHWT's arm IK lands the palm with its target's orientation: the sticks are in the hand (BandAnimator.Solve)
            for side in 'lr':
                pm, tg = N['bone_palm_' + side], N['bone_ik_hand_slave_' + side]
                M = rig.W[pm].copy()
                M[:3, :3] = rig.W[tg][:3, :3]
                rig.set_world(pm, M)
        frames.append(skinned(meshes, [m @ b[4] for m, b in zip(rig.W, bones)]))
        centre.append(rig.W[N['bone_pelvis']][:3, 3])
    c = np.mean(centre, axis=0)
    binp = os.path.join(work, name + '.bin')
    ca, sa = math.cos(PITCH), math.sin(PITCH)
    with open(binp, 'wb') as o:
        o.write(struct.pack('<ii', len(meshes), FRAMES))
        for tex, V, T in meshes:
            o.write(tex.encode()[:32].ljust(32, b'\0'))
            o.write(struct.pack('<ii', len(V), len(T)))
            o.write(np.asarray(V['uv'], '<f4').tobytes())
            o.write(np.asarray(T, '<u4').tobytes())
        for fr in frames:
            for p, n in fr:
                x, y, z = p[:, 0] - c[0], p[:, 1], p[:, 2] - c[2]
                nx, ny, nz = n[:, 0], n[:, 1], n[:, 2]
                ln = np.sqrt(nx * nx + ny * ny + nz * nz) + 1e-9
                # y' up the picture, z' away: the picture's depth for GL is -z'
                a = np.stack([x, y * ca + z * sa, -(z * ca - y * sa), nx / ln, ny / ln, -nz / ln], 1)
                o.write(np.asarray(a, '<f4').tobytes())
    pre = os.path.join(work, name)
    CW, CH, PPM, Y0 = CELLS.get(role, STAND)
    subprocess.run([RENDER, tex_dir, binp, pre, str(CW * SS), str(CH * SS), str(PPM * SS), str(Y0)], check=True)
    os.remove(binp)
    sheet = Image.new('RGBA', (CW * COLS, CH * ROWS), (0, 0, 0, 0))
    for i in range(FRAMES):
        im = Image.open('%s_%02d.png' % (pre, i)).convert('RGBa').resize((CW, CH), Image.BOX).convert('RGBA')
        sheet.paste(im, ((i % COLS) * CW, (i // COLS) * CH))
    out = os.path.join(OUT, name + '.png')
    sheet.quantize(256, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE).save(out, optimize=True)
    print(name, clip[0], 'from frame', s0, 'span', span, os.path.getsize(out) // 1024, 'KB')


def main():
    tex_dir, work = sys.argv[1], sys.argv[2]
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(work, exist_ok=True)
    picks = sys.argv[3:]
    for p in sorted(glob.glob(os.path.join(tex_dir, 'band_*.wtch'))):
        if not picks or os.path.basename(p)[5:-5] in picks:
            bake(p, work, tex_dir)
    if not picks:
        with open(os.path.join(OUT, 'band.txt'), 'w') as f:
            f.write('\n'.join(sorted(x[:-4] for x in os.listdir(OUT) if x.endswith('.png'))) + '\n')


main()
