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
spec = importlib.util.spec_from_file_location('wc', os.path.join(os.environ.get('GHWT_TOOLS') or os.path.expanduser('~/mods/refs/yarg/ghwt-tools'), 'wtch_check.py'))
wc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wc)

FRAMES, COLS, ROWS = 48, 8, 6
V_FRAMES, V_ROWS = 64, 8   # every sheet: 48 as before, then 16 more. Singer: 32 singing (t100), 16 between lines
# (ns_100), 16 of the Star Power jump (spjump). Guitars: 48 groove x strum, 16 of the jump. Drummer: 48 strikes, 8 of
# GHWT's idle (between parts), 8 of armsup (the last chord)
REST, JUMP = 32, 48
SING = 32               # the singer's sheet: this many frames of the loop with the mic at the mouth, the rest with it down
CELLS = {'drummer': (266, 232, 88, -0.62)}
STAND = (255, 384, 180, -0.08)      # cell w, h, pixels per metre, the cell's bottom in metres
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


# the singer kept near upright: GHWT's singing loops (the wilder ones above all) lean the body back up to 37 degrees with
# the head thrown back, which from the camera's height is a body lying across the shot. The upper body is turned about
# the hips until the head is within SING_LEAN of over them; the hair, the hats and what hangs on the chest are bones of
# the pelvis's in these models (Bone_ACC_*), so they're turned with it or they'd stay behind in the air
SING_LEAN = 12.0
STAND_AHEAD = 0.42       # the mic stand this far in front of the singer (toward the audience, -z)


def straighten(rig, N):
    p = rig.W[N['bone_pelvis']][:3, 3].copy()
    v = rig.W[N['bone_head']][:3, 3] - p
    up = np.array([0, 1.0, 0])
    tilt = math.degrees(math.atan2(math.hypot(v[0], v[2]), v[1]))
    if tilt <= SING_LEAN:
        return
    axis = np.cross(v, up)
    if np.linalg.norm(axis) < 1e-6:
        return
    axis /= np.linalg.norm(axis)
    a = math.radians(tilt - SING_LEAN)
    K = np.array([[0, -axis[2], axis[1]], [axis[2], 0, -axis[0]], [-axis[1], axis[0], 0]])
    R = np.eye(3) + math.sin(a) * K + (1 - math.cos(a)) * K @ K
    T = np.eye(4)
    T[:3, :3] = R
    T[:3, 3] = p - R @ p
    moved = set(rig.subtree(N['bone_stomach_lower']))
    for name, i in N.items():
        if name.lower().startswith(('bone_acc_head', 'bone_acc_torso')):
            moved.update(rig.subtree(i))
    for k in moved:
        rig.W[k] = T @ rig.W[k]


HEAD_TILT = 25.0        # the head against the chest, at most (a head thrown right back read as a broken neck)


def level_head(rig, N, rest):
    h, c = N['bone_head'], N['bone_chest']
    hu = rig.W[h][:3, :3] @ rest[h][:3, :3].T @ np.array([0, 1.0, 0])
    cu = rig.W[c][:3, :3] @ rest[c][:3, :3].T @ np.array([0, 1.0, 0])
    ang = math.degrees(math.acos(float(np.clip(hu @ cu, -1, 1))))
    if ang <= HEAD_TILT:
        return
    axis = np.cross(hu, cu)
    if np.linalg.norm(axis) < 1e-6:
        return
    axis /= np.linalg.norm(axis)
    a = math.radians(ang - HEAD_TILT)
    K = np.array([[0, -axis[2], axis[1]], [axis[2], 0, -axis[0]], [-axis[1], axis[0], 0]])
    R = np.eye(3) + math.sin(a) * K + (1 - math.cos(a)) * K @ K
    p = rig.W[h][:3, 3].copy()
    T = np.eye(4)
    T[:3, :3] = R
    T[:3, 3] = p - R @ p
    moved = set(rig.subtree(h))
    for name, i in N.items():
        if name.lower().startswith('bone_acc_head'):
            moved.update(rig.subtree(i))
    for k in moved:
        rig.W[k] = T @ rig.W[k]


# the singer turned back toward the audience: GHWT's singing loops swing the chest up to 50 degrees round (an idle
# one nearly 90), and a camera off to one side then had the back of her head. A quarter of the turn is kept
SING_TURN_KEPT = 0.25


def face_front(rig, rest, N):
    ch = N['bone_chest']
    f = rest[ch][:3, :3].T @ np.array([0, 0, -1.0])      # the chest's own forward, from the rest pose
    w = rig.W[ch][:3, :3] @ f
    if math.hypot(w[0], w[2]) < 0.4:
        return                              # the chest faces up or down: no way round to speak of
    yaw = math.atan2(w[0], -w[2])
    a = -(1 - SING_TURN_KEPT) * yaw
    c, s = math.cos(a), math.sin(a)
    R = np.array([[c, 0, s], [0, 1.0, 0], [-s, 0, c]])
    # check the sense: turned by R the chest must face nearer -z, else the other way round
    w2 = R @ w
    if abs(math.atan2(w2[0], -w2[2])) > abs(yaw):
        R = R.T
    piv = rig.W[N['bone_pelvis']][:3, 3].copy()
    T = np.eye(4)
    T[:3, :3] = R
    T[:3, 3] = piv - R @ piv
    for k in range(len(rig.W)):             # every bone (the mic and the IK targets with them)
        rig.W[k] = T @ rig.W[k]


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


def hand_pose(anims, bass, male, shape, k, chord=None, fret=None):
    # the chord, fret and strum of one cell: what HandPlayer drives from the chart
    if chord is None:
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


def finger_pose(anims, male, mask):
    # the hand's bones with the fingers of `mask` down (each from the roll that brings it down) and the rest lifted
    kind = 'male' if male else 'fmale'
    over = {b: x[0] for b, x in anims['gh3_hnd_guit_chord_%s_empty' % kind][1].items() if 'ik_hand' not in b}
    top = 0
    for k, fg in enumerate(FINGERS):
        if mask & (1 << k):
            top = k + 1
            for b, x in anims['gh3_hnd_guit_chord_%s_roll3_%d' % (kind, k + 1)][1].items():
                if fg in b:
                    over[b] = x[0]
    if top:
        for b, x in anims['gh3_hnd_guit_chord_%s_roll3_%d' % (kind, top)][1].items():
            if 'thumb' in b:
                over[b] = x[0]
    return over


# where each finger's tip goes on the neck (the guitar body's space: the neck along +z, the fretboard's face +y, the
# strings across x, the fingers coming round the +x edge): GH's colours are fingers, and a finger down is a finger on a
# string at its fret, not curled in the air beside the neck. Index to little finger on the strings from the low side up,
# as a power chord or a run across the strings looks
PRESS_STRING = [2, 3, 3, 4]         # bone_guitar_string_<n + 1>
LIFT = 0.018                        # a finger that's up hovers this far over the strings, ready


def press_fingers(rig, mask):
    gi = rig.ix('bone_guitar_body')
    G = rig.W[gi]
    Gi = np.linalg.inv(G)
    loc = lambda p: (Gi @ np.r_[p, 1])[:3]
    world = lambda q: (G @ np.r_[q, 1])[:3]
    sx = [loc(rig.p('bone_guitar_string_%d' % (k + 1))) for k in range(6)]
    top_y = max(v[1] for v in sx)
    for k, fg in enumerate(FINGERS):
        nm = lambda part: 'Bone_Hand_%s_%s_L' % ({'pinkey': 'Pinkey'}.get(fg, fg.capitalize()), part)
        base, mid, top = rig.ix(nm('Base')), rig.ix(nm('Mid')), rig.ix(nm('Top'))
        B, Mj, T = rig.W[base][:3, 3], rig.W[mid][:3, 3], rig.W[top][:3, 3]
        lt = 0.8 * np.linalg.norm(T - Mj)                   # the last joint to the tip
        tip_now = loc(T + (T - Mj) / max(np.linalg.norm(T - Mj), 1e-6) * lt)
        st = sx[PRESS_STRING[k]]
        down = mask & (1 << k)
        # the tip: on its string just behind where it is now along the neck (the hand's own spread of the fingers)
        tip = np.array([st[0], top_y + (0.004 if down else LIFT), tip_now[2]])
        if not down:
            tip[0] = st[0] + 0.006                          # an idle finger hovers over its string, ready
        # each joint in turn turned to bring the tip toward its place (from the hand's own curl, so the finger keeps a
        # finger's shape), a few times round
        target = world(tip)
        chain = [top, mid, base]
        for _ in range(12):
            for j in chain:
                T, Mj = rig.W[top][:3, 3], rig.W[mid][:3, 3]
                end = T + (T - Mj) / max(np.linalg.norm(T - Mj), 1e-6) * lt
                J = rig.W[j][:3, 3]
                u, v = end - J, target - J
                if np.linalg.norm(u) < 1e-5 or np.linalg.norm(v) < 1e-5:
                    continue
                R = rotation_between(u, v)
                # a joint turns at most 25 degrees a pass: no snapping backwards through the knuckle
                ang = np.arccos(np.clip((np.trace(R) - 1) / 2, -1, 1))
                if ang > np.radians(25):
                    axis = np.cross(u, v)
                    axis /= max(np.linalg.norm(axis), 1e-9)
                    t = np.radians(25)
                    K = np.array([[0, -axis[2], axis[1]], [axis[2], 0, -axis[0]], [-axis[1], axis[0], 0]])
                    R = np.eye(3) + np.sin(t) * K + (1 - np.cos(t)) * K @ K
                rig.rotate_world(j, R)


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


# the fret hand as a player holds a neck (the guitar body's space: the neck along +z, the board's face +y, the floor
# side +x). GHWT's target put the palm through the neck with the fingers bunched in the air over it. Here the hand is
# placed outright: its knuckles along the floor-side edge just over the board, the fingers pointing up and across it
# (leaning over the strings), index toward the head; the arm reaches to it and the fingers then go to their strings
KNUCKLE_AT = np.array([0.05, 0.022, -0.012])
FINGER_DIR = np.array([-0.8, 1.0, 0.0])


def place_fret_hand(rig):
    gi = rig.ix('bone_guitar_body')
    G = rig.W[gi]
    pm = rig.ix('bone_palm_l')
    P = rig.W[pm]
    loc = lambda n: P[:3, :3].T @ (rig.W[rig.ix(n)][:3, 3] - P[:3, 3])
    a_l = loc('Bone_Hand_Middle_Base_L')
    k_l = loc('Bone_Hand_Pinkey_Base_L') - loc('Bone_Hand_Index_Base_L')

    def frame(a, k):
        e1 = a / np.linalg.norm(a)
        e2 = k - (k @ e1) * e1
        e2 /= np.linalg.norm(e2)
        return np.c_[e1, e2, np.cross(e1, e2)]
    D = frame(G[:3, :3] @ FINGER_DIR, G[:3, :3] @ np.array([0, 0, -1.0]))
    R = D @ frame(a_l, k_l).T
    fret = rig.W[rig.ix('BONE_GUITAR_FRET_POS')][:3, 3]
    knuckle = fret + G[:3, :3] @ KNUCKLE_AT
    at = knuckle - R @ a_l
    el = rig.p('bone_forearm_l')
    rig.two_bone('bone_bicep_l', 'bone_forearm_l', 'bone_palm_l', at, el + np.array([0, -0.12, 0]))
    M = rig.W[pm].copy()
    M[:3, :3] = R
    M[:3, 3] = at
    rig.set_world(pm, M)


def guitar_hands(rig):
    # BandAnimator.Solve: elbows a little down, each palm exactly on its target, rotation included (the fret hand then
    # placed on the neck)
    for side, fix in (('r', FIX_R), ('l', FIX_L)):
        tgt = rig.ix('bone_ik_hand_guitar_' + side)
        el = rig.p('bone_forearm_' + side)
        rig.two_bone('bone_bicep_' + side, 'bone_forearm_' + side, 'bone_palm_' + side, rig.W[tgt][:3, 3],
                     el + np.array([0, -0.12, 0]))
        pm = rig.ix('bone_palm_' + side)
        M = rig.W[pm].copy()
        M[:3, :3] = rig.W[tgt][:3, :3] @ fix
        rig.set_world(pm, M)
    place_fret_hand(rig)


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


ALT = {'guitarist': ['horse_100', 'jimi_100', 'areyou_100', 'joker_100', 'bleed_100', 'damnit_100'],
       'bassist': ['joker_100', 'bleed_100', 'danjoker_100', 'horse_100', 'jimi_100'],
       'vocalist': ['today_100', 'areyou_100', 'dammit_100']}


THREED = False          # --3d: the member as a model for the game to draw (assets/band3d), not a picture sheet
TRI_BUDGET = int(os.environ.get('BAND_TRIS', 16000))       # a member's triangles after thinning: a close shot shows them a screen tall
DRUM_BUDGET = int(os.environ.get('DRUM_TRIS', 22000))     # the drummer's, the kit included
ATLAS = 1024            # a member's textures packed into one picture this size: one draw for the lot
HEAD_DENSITY, BODY_DENSITY = 1700.0, 430.0     # texels a metre the atlas gives a face, and the rest
BLENDER = os.environ.get('BLENDER') or os.path.expanduser('~/mods/refs/blender-4.2.3-linux-x64/blender')    # 4.0 does as well
HEAD_WORDS = ('head', 'jaw', 'eye', 'brow', 'lip', 'cheek', 'hair', 'face', 'nose', 'ear', 'tongue', 'teeth', 'mouth')
# the fret hand's layers (game.c drives them from the chart): each groove step with the hand at these frets of the
# neck, in these chord shapes (one finger, another finger, a barre, the hand open)
LAYER_FRETS = [1, 6, 11, 16]
LAYER_CHORDS = ['empty']
# the fret hand's fingers, as GH3 has them: a finger a colour (green the index, red the middle, yellow the ring, blue
# the little one; orange is the little one with the hand a place up the neck), so a chord is its colours' fingers down.
# One pose of the hand's bones for each of the 16 sets of fingers, kept relative to the palm: the game hangs them on
# the palm wherever the layers put it
FINGERS = ['index', 'middle', 'ring', 'pinkey']


def find_tex(tex_dir, name):
    p = os.path.join(tex_dir, name + '.png')
    if os.path.exists(p):
        return p
    if len(name) == 31:
        for f in sorted(os.listdir(tex_dir)):
            if f.startswith(name) and f.endswith('.png'):
                return os.path.join(tex_dir, f)
    return None


def thin(meshes, bones, budget, work, name, SMALL=400, PROP=0.85, HEAD=1.9, FLOOR=0.2):
    # Blender's Decimate on every mesh big enough to matter (it keeps texture places and bone weights through the
    # collapse); the small ones (sticks, the mic, cymbals, eyes) whole. A face keeps most of its triangles
    total = sum(len(T) for _, _, T in meshes)
    head, prop = [], []
    for tex, V, T in meshes:
        w = np.zeros(len(bones))
        np.add.at(w, np.asarray(V['j']).ravel(), np.asarray(V['w']).ravel())
        hw = sum(w[i] for i, b in enumerate(bones) if any(k in b[0].lower() for k in HEAD_WORDS))
        head.append(hw > 0.6 * max(w.sum(), 1e-9))
        # the kit, a guitar, the mic and its stand: flat and thin things a collapse eats (a cymbal goes to nothing)
        pw = sum(w[i] for i, b in enumerate(bones) if any(k in b[0].lower() for k in ('control_root', 'guitar', 'mic_')))
        prop.append(pw > 0.4 * max(w.sum(), 1e-9))
    kept = sum(len(T) * PROP for (_, _, T), p in zip(meshes, prop) if p and len(T) > SMALL)
    small = sum(len(T) for _, _, T in meshes if len(T) <= SMALL)
    rest = total - small - kept / PROP
    ratio = min(1.0, max(budget - small - kept, rest * FLOOR) / max(rest, 1))
    arrs = {'count': len(meshes)}
    for i, (tex, V, T) in enumerate(meshes):
        r = 1.0 if len(T) <= SMALL else PROP if prop[i] else min(1.0, ratio * HEAD) if head[i] else ratio * 0.9
        arrs.update({'p%d' % i: np.asarray(V['p'], np.float32), 'uv%d' % i: np.asarray(V['uv'], np.float32),
                     'j%d' % i: np.asarray(V['j'], np.int32), 'w%d' % i: np.asarray(V['w'], np.float32),
                     'a%d' % i: np.asarray(V['a'], np.float32), 't%d' % i: np.asarray(T, np.int32), 'r%d' % i: r})
    src, dst = os.path.join(work, name + '_in.npz'), os.path.join(work, name + '_out.npz')
    np.savez(src, **arrs)
    subprocess.run([BLENDER, '-b', '--python', os.path.join(HERE, 'band_decimate_bl.py'), '--', src, dst], check=True,
                   stdout=subprocess.DEVNULL)
    d = np.load(dst)
    out = []
    for i, (tex, V, T) in enumerate(meshes):
        if len(d['t%d' % i]):
            out.append((tex, {k: d['%s%d' % (k, i)] for k in ('p', 'n', 'uv', 'j', 'w', 'a')}, d['t%d' % i], head[i]))
    os.remove(src)
    os.remove(dst)
    return out


def pack_atlas(parts, tex_dir, name, out, ATLAS=ATLAS, dens=1.0, tag='_atlas', PAD=3, ALONE=None):
    # every mesh's texture into one picture (ATLAS square): a mesh whose texture places run past the texture's edge (a
    # mirrored body: -1..1) gets the texture laid out as many times as it reaches, so nothing needs to wrap. What reaches
    # further than twice each way stays a part of its own with a small wrapping texture
    rects, alone = [], []
    for pi, (tex, V, T, head) in enumerate(parts):
        src = find_tex(tex_dir, tex) if tex else None
        uv = V['uv']
        u0, v0 = math.floor(uv[:, 0].min() + 1e-3), math.floor(uv[:, 1].min() + 1e-3)
        nu, nv = max(1, math.ceil(uv[:, 0].max() - 1e-3) - u0), max(1, math.ceil(uv[:, 1].max() - 1e-3) - v0)
        if not src or nu > 2 or nv > 2:
            alone.append(pi)
            continue
        im = Image.open(src)
        P = V['p']
        e1, e2 = P[T[:, 1]] - P[T[:, 0]], P[T[:, 2]] - P[T[:, 0]]
        a3 = np.linalg.norm(np.cross(e1, e2), axis=1).sum() / 2
        f1, f2 = uv[T[:, 1]] - uv[T[:, 0]], uv[T[:, 2]] - uv[T[:, 0]]
        a2 = np.abs(f1[:, 0] * f2[:, 1] - f1[:, 1] * f2[:, 0]).sum() / 2
        m_per_uv = math.sqrt(a3 / max(a2, 1e-9))            # metres one whole texture covers on them
        want = m_per_uv * (HEAD_DENSITY if head else BODY_DENSITY) * dens
        w = int(min(max(want, 8), im.width)) * nu
        h = int(min(max(want * im.height / im.width, 8), im.height)) * nv
        rects.append([pi, src, (u0, v0, nu, nv), w, h, (src, nu, nv)])
    # the same texture over the same tiles: one place for all the meshes that use it
    shared = {}
    for r in rects:
        k = r[5]
        if k in shared:
            shared[k][3], shared[k][4] = max(shared[k][3], r[3]), max(shared[k][4], r[4])
        else:
            shared[k] = [None, r[1], r[2], r[3], r[4]]
    scale = 1.0
    while True:
        items = sorted(shared.items(), key=lambda kv: -kv[1][4])
        x = y = row = 0
        place, ok = {}, True
        for k, r in items:
            w, h = max(4, int(r[3] * scale)), max(4, int(r[4] * scale))
            if x + w + 2 * PAD > ATLAS:
                x, y, row = 0, y + row, 0
            if y + h + 2 * PAD > ATLAS or w + 2 * PAD > ATLAS:
                ok = False
                break
            place[k] = (x + PAD, y + PAD, w, h)
            x += w + 2 * PAD
            row = max(row, h + 2 * PAD)
        if ok:
            break
        scale *= 0.94
    used_h = y + row
    AH = 1 << max(6, math.ceil(math.log2(max(used_h, 1))))
    atlas = Image.new('RGBA', (ATLAS, AH), (0, 0, 0, 0))
    for k, (x, y, w, h) in place.items():
        src, nu, nv = k
        im = Image.open(src).convert('RGBA')
        tw, th = max(1, w // nu), max(1, h // nv)
        tile = im.resize((tw, th), Image.LANCZOS)
        # the tiles, and a margin of the texture going on round them (what a wrap would have shown)
        big = Image.new('RGBA', (tw * (nu + 2), th * (nv + 2)))
        for a in range(nu + 2):
            for b in range(nv + 2):
                big.paste(tile, (a * tw, b * th))
        big = big.crop((tw - PAD, th - PAD, tw * (nu + 1) + PAD, th * (nv + 1) + PAD))
        atlas.paste(big, (x - PAD, y - PAD))
        place[k] = (x, y, tw * nu, th * nv)
    aname = name + tag
    atlas.save(os.path.join(out, 'tex', aname + '.png'), optimize=True)
    # where each texture went (the hair's places, for a dye: export3d)
    pack_atlas.last = {'atlas': aname, 'h': AH, 'place': {k[0]: v for k, v in place.items()}, 'alone': {}}
    # the atlas's meshes as one part, the rest as they were
    merged = {k: [] for k in ('p', 'n', 'uv', 'j', 'w', 'a')}
    tris, base = [], 0
    for pi, src, box, w, h, key in rects:
        tex, V, T, head = parts[pi]
        x, y, pw, ph = place[key]
        uv = V['uv'].copy()
        uv[:, 0] = (x + (uv[:, 0] - box[0]) / box[2] * pw) / ATLAS
        uv[:, 1] = (y + (uv[:, 1] - box[1]) / box[3] * ph) / AH
        for k in merged:
            merged[k].append(uv if k == 'uv' else V[k])
        tris.append(T + base)
        base += len(V['p'])
    result = []
    if base:
        result.append((aname, {k: np.concatenate(v) for k, v in merged.items()}, np.concatenate(tris), False))
    for pi in alone:
        tex, V, T, head = parts[pi]
        src = find_tex(tex_dir, tex) if tex else None
        tname = ''
        if src:
            tname = (name + ('f_' if dens < 1 else '_') + os.path.basename(src)[:-4])[-31:]
            im = Image.open(src).convert('RGBA')
            k = max(im.width, im.height) / (ALONE or (128 if dens >= 1 else 32))
            if k > 1:
                im = im.resize((max(1, int(im.width / k)), max(1, int(im.height / k))), Image.LANCZOS)
            im.save(os.path.join(out, 'tex', tname + '.png'), optimize=True)
            pack_atlas.last['alone'][src] = tname
        result.append((tname, V, T, True))          # True: wraps
    print(name, 'atlas', ATLAS, 'x', AH, 'scale %.2f' % scale, len(place), 'textures,', len(alone), 'parts alone')
    return result


FAR_BUDGET, FAR_DRUM = 2200, 4200       # the far body's triangles: a member a fifth of the screen tall
FAR_ATLAS = 256


def part_records(parts, name):
    recs = []
    for tname, V, T, wraps in parts:
        n = V['n'] / (np.linalg.norm(V['n'], axis=1, keepdims=True) + 1e-9)
        w = V['w'] / np.maximum(V['w'].sum(1, keepdims=True), 1e-6)
        wq = np.round(w * 255).astype(np.int32)
        wq[:, 0] += 255 - wq.sum(1)
        rec = np.zeros(len(V['p']), np.dtype([('p', '<f4', 3), ('n', '<f4', 3), ('uv', '<f4', 2), ('j', 'u1', 4), ('w', 'u1', 4)]))
        rec['p'] = V['p']
        rec['n'] = n
        rec['uv'] = V['uv']
        rec['j'][:, :2] = V['j']
        rec['w'][:, :2] = np.clip(wq, 0, 255)
        rec['w'][:, 3] = np.clip(np.round(V['a'] * 255), 0, 255)       # the vertex's own alpha
        # 65535 vertices a part at most (u16 indices)
        assert len(rec) < 65536, name
        recs.append((tname, rec, np.asarray(T, '<u2'), wraps))
    return recs


def write_parts(o, recs):
    for tname, rec, T1, wraps in recs:
        o.write(tname.encode().ljust(32, b'\0'))
        o.write(struct.pack('<iii', len(rec), len(T1), 1 if wraps else 0))
        o.write(rec.tobytes())
        o.write(T1.tobytes())


def no_soft(meshes, tex_dir, bones):
    # World Tour lays soft shadows over a face (across the eyeball under the lid, in the eye's socket): triangles whose
    # texture is half clear all over. The game draws a model solid or not at all (an alpha test), which made slabs of
    # them: white and black over the iris, dark patches round the eyes. They go; what's cut out sharp (lashes, brows,
    # hair) has clear and solid texels, never half ones all over, and stays
    out, cut = [], 0
    for tex, V, T in meshes:
        src = find_tex(tex_dir, tex) if tex else None
        T = np.asarray(T)
        if src is None or len(T) == 0:
            out.append((tex, V, T))
            continue
        a = np.asarray(Image.open(src).convert('RGBA'))[:, :, 3].astype(np.float32) / 255
        if ((a > 0.06) & (a < 0.92)).mean() < 0.02:
            out.append((tex, V, T))
            continue
        h, w = a.shape
        uv = np.asarray(V['uv'], np.float32)[T]                 # tris x 3 x 2
        pts = [uv[:, 0], uv[:, 1], uv[:, 2], uv.mean(1), (uv[:, 0] + uv[:, 1]) / 2, (uv[:, 1] + uv[:, 2]) / 2, (uv[:, 2] + uv[:, 0]) / 2]
        # only on a face: a guitar's strings are thin soft lines too, and they stay
        wt = np.zeros(len(bones))
        np.add.at(wt, np.asarray(V['j']).ravel(), np.asarray(V['w']).ravel())
        if sum(wt[i] for i, b in enumerate(bones) if any(k in b[0].lower() for k in HEAD_WORDS)) < 0.6 * max(wt.sum(), 1e-9):
            out.append((tex, V, T))
            continue
        # soft: nowhere solid (a shadow's middle is clear, so "half clear all over" misses it), and by an eye: hair is
        # soft cards too, and it stays
        P = np.asarray(V['p'], np.float32)[T].mean(1)
        eyes = [np.linalg.inv(b[4])[:3, 3] for b in bones if b[0].lower() in ('bone_eye_l', 'bone_eye_r')]
        soft = np.zeros(len(T), bool)
        for e in eyes:
            soft |= np.linalg.norm(P - e, axis=1) < 0.05
        for p in pts:
            s = a[np.clip((np.mod(p[:, 1], 1) * h).astype(int), 0, h - 1), np.clip((np.mod(p[:, 0], 1) * w).astype(int), 0, w - 1)]
            soft &= s < 0.92
        cut += int(soft.sum())
        if (~soft).any():
            out.append((tex, V, T[~soft]))
    return out, cut


FACE_BONES = ('jaw', 'lip', 'mouth', 'eye', 'brow', 'cheek', 'nostril', 'tongue')


def hair_textures(meshes, bones, tex_dir):
    # the textures of what's on the head that isn't the face: hair, a hat, a band in the hair. The face's textures are
    # the ones any mesh moved by the jaw, lips, eyes or brows uses (a face painted with its hairline stays the face's)
    eye = [np.linalg.inv(b[4])[:3, 3] for b in bones if b[0].lower() in ('bone_eye_l', 'bone_eye_r')]
    eye_y = float(np.mean([e[1] for e in eye])) if eye else 1.6
    near_head = set()
    for i, b in enumerate(bones):
        n = b[0].lower()
        y = float(np.linalg.inv(b[4])[1, 3])
        if 'head' in n or ('acc' in n and y > eye_y - 0.3):
            near_head.add(i)
    face, cand = set(), []
    for tex, V, T in meshes:
        src = find_tex(tex_dir, tex) if tex else None
        if not src or len(T) == 0:
            continue
        w = np.zeros(len(bones))
        np.add.at(w, np.asarray(V['j']).ravel(), np.asarray(V['w']).ravel())
        tot = max(w.sum(), 1e-9)
        if sum(w[i] for i, b in enumerate(bones) if any(k in b[0].lower() for k in FACE_BONES)) > 0.1 * tot:
            face.add(src)
        elif sum(w[i] for i in near_head) > 0.6 * tot:
            cand.append(src)
    return set(c for c in cand if c not in face)


def write_hair(out, hair):
    # tex/<atlas>.hair: the hair's places in that atlas (x y w h in its pixels), or "all <texture>" for one of its own
    info = pack_atlas.last
    lines = ['%d %d %d %d' % info['place'][src] for src in hair if src in info['place']]
    lines += ['all %s' % info['alone'][src] for src in hair if src in info['alone']]
    path = os.path.join(out, 'tex', info['atlas'] + '.hair')
    if lines:
        open(path, 'w').write('\n'.join(lines) + '\n')
    elif os.path.exists(path):
        os.remove(path)


FINGER_TABLE = {}       # member -> (the palm's bone, the fingers' bones, 16 x bones x 3 x 4)


def export3d(name, meshes, skins, c, alt, tex_dir, layers, bones, work):
    # assets/band3d/<name>.b3d: the mesh (thinned, bone weights kept, textures in one atlas), every frame's skin matrices
    # in the sheet's order, and the fret hand's layers; alt/<name>.b3a: the wilder loop's matrices and layers for the
    # same mesh. Model space: x from the loop's centre, y up from the feet, facing -z
    out = os.path.join(HERE, '..', 'assets', 'band3d')
    os.makedirs(os.path.join(out, 'tex'), exist_ok=True)
    os.makedirs(os.path.join(out, 'alt'), exist_ok=True)
    S = np.stack([np.stack(sk)[:, :3, :] for sk in skins]).astype('<f4')      # frames x bones x 3 x 4
    S[:, :, 0, 3] -= c[0]      # the loop's centre at x = z = 0, as each sheet is centred on its own loop
    S[:, :, 2, 3] -= c[2]
    lb, L = (np.zeros(0, '<i4'), np.zeros((0, 0, 0, 0, 3, 4), '<f4')) if layers is None else layers
    L = L.astype('<f4').copy()
    if L.size:
        L[..., 0, 3] -= c[0]
        L[..., 2, 3] -= c[2]
    lhead = struct.pack('<iiii', len(lb), L.shape[0] if L.size else 0, L.shape[1] if L.size else 0,
                        L.shape[2] if L.size else 0)
    if alt:
        with open(os.path.join(out, 'alt', name + '.b3a'), 'wb') as o:
            o.write(b'B3A2' + struct.pack('<ii', S.shape[1], S.shape[0]) + lhead)
            o.write(S.tobytes())
            o.write(np.asarray(lb, '<i4').tobytes())
            o.write(L.tobytes())
        print(name, '3d alt', S.shape[0], 'frames', len(lb), 'hand bones')
        return
    meshes, cut = no_soft(meshes, tex_dir, bones)
    total = sum(len(T) for _, _, T in meshes)
    print(name, cut, 'soft shadow triangles left out')
    parts = thin(meshes, bones, DRUM_BUDGET if name.startswith('drummer') else TRI_BUDGET, work, name)
    hair = hair_textures(meshes, bones, tex_dir)
    parts = pack_atlas(parts, tex_dir, name, out)
    write_hair(out, hair)
    recs = part_records(parts, name)
    # the far body: a quarter of the triangles and a small atlas with no mipmaps (an atlas's mipmaps run its pictures
    # into each other, which is what a small figure would be drawn with)
    drum = name.startswith('drummer')
    far = thin(meshes, bones, FAR_DRUM if drum else FAR_BUDGET, work, name + '_far', 60, 0.3, 1.3, 0.07)
    far = pack_atlas(far, tex_dir, name, out, FAR_ATLAS, 0.25, '_far', 2)
    write_hair(out, hair)
    far = part_records(far, name)
    os.makedirs(os.path.join(out, 'far'), exist_ok=True)
    with open(os.path.join(out, 'far', name + '.b3d'), 'wb') as o:
        o.write(b'B3F1' + struct.pack('<i', len(far)))
        write_parts(o, far)
    print(name, 'far', sum(len(r) for _, r, _, _ in far), 'verts', sum(len(t) for _, _, t, _ in far), 'tris', len(far), 'parts')
    with open(os.path.join(out, name + '.b3d'), 'wb') as o:
        o.write(b'B3D2' + struct.pack('<iii', len(recs), S.shape[1], S.shape[0]) + lhead)
        write_parts(o, recs)
        o.write(S.tobytes())
        o.write(np.asarray(lb, '<i4').tobytes())
        o.write(L.tobytes())
        # what a camera aims at: the head, the chest and the pelvis (the bone, and where its joint is in the mesh)
        low = {b[0].lower(): i for i, b in enumerate(bones)}
        o.write(struct.pack('<i', 3))
        for bn in ('bone_head', 'bone_chest', 'bone_pelvis'):
            i = low.get(bn, 0)
            o.write(struct.pack('<i3f', i, *np.linalg.inv(bones[i][4])[:3, 3]))
        # the fret hand's fingers (guitars and basses): the palm, the bones, 16 poses
        if name in FINGER_TABLE:
            palm, fb, C = FINGER_TABLE[name]
            o.write(b'FNG1' + struct.pack('<ii', palm, len(fb)) + fb.tobytes() + C.tobytes())
    print(name, '3d', sum(len(r) for _, r, _, _ in recs), 'verts', sum(len(t) for _, _, t, _ in recs), 'tris (from', total,
          ')', len(recs), 'parts', len(lb), 'hand bones', os.path.getsize(os.path.join(out, name + '.b3d')) // 1024, 'KB')


def guitar_swing(bones, c, N, start, sp):
    # how far the guitar turns against the chest over a loop, in degrees (the worst frame against the first)
    ref = None
    worst = 0.0
    for k in range(start, start + sp, max(1, sp // 16)):
        M = wc.pose(bones, c, k)
        rel = np.linalg.inv(M[N['bone_chest']]) @ M[N['bone_guitar_body']]
        if ref is None:
            ref = rel
        cosang = (np.trace(ref[:3, :3].T @ rel[:3, :3]) - 1) / 2
        worst = max(worst, float(np.degrees(np.arccos(np.clip(cosang, -1, 1)))))
    return worst


def bake(path, work, tex_dir, alt=False):
    # alt: the member's other GHWT loop (the wilder one, for choruses and solos) into assets/band/alt/<name>.png: the
    # guitars' 48 groove frames, the singer's 32 singing ones, laid out as the main sheet's first rows
    name = os.path.basename(path)[5:-5]
    role = name.split('_')[0]
    bones, meshes, clips = wc.read(path)
    if THREED:
        # the file's texture places count up from the picture's bottom row, the game's down from its top: turned over
        # here (they went on upside down before: nostrils on the forehead, the eye's white where its iris should be)
        flipped = []
        for t, V, T in meshes:
            V = V.copy()
            V['uv'][:, 1] = 1 - V['uv'][:, 1]
            flipped.append((t, V, T))
        meshes = flipped
    N = {b[0]: i for i, b in enumerate(bones)}
    rest = wc.rest_model(bones)
    clip = pick_clip(clips, role)
    if alt:
        byname0 = {c[0]: c for c in clips}
        want = [n for n in ALT.get(role, []) if n in byname0]
        if not want:
            print(name, 'no other loop')
            return
        clip = byname0[want[0]]
    span = int(round(BEATS * 60 / 100 * clip[1]))
    s0 = loop_start(bones, clip, N, span, role)
    if role in ('guitarist', 'bassist') and alt:
        # the wilder loop is only worth having if the guitar stays where a player can fret it: one that throws the
        # neck about is left out, and the steady loop plays the choruses too
        sp = int(round(BEATS * 60 / 100 * clip[1]))
        w = guitar_swing(bones, clip, N, s0, sp)
        print(name, 'other loop', clip[0], 'guitar swing %.0f deg' % w)
        if w > 12:
            print(name, 'left out')
            for f in (os.path.join(OUT, 'alt', name + '.png'), os.path.join(HERE, '..', 'assets', 'band3d', 'alt', name + '.b3a')):
                if os.path.exists(f):
                    os.remove(f)
            return
    if role in ('guitarist', 'bassist') and not alt:
        # the loop that holds the guitar in playing position: World Tour's own "t100" swings some guitars from flat to
        # upright and back every couple of beats (it's made for being seen from the crowd), and a hand fretting the
        # chart's notes on a neck that's pointing at the ceiling looks like nothing at all. Of the 100 bpm loops, the
        # one whose four beats keep the neck steadiest on the body
        def swing(c, start):
            return guitar_swing(bones, c, N, start, int(round(BEATS * 60 / 100 * c[1])))
        best = (swing(clip, s0), clip, s0)
        for c in clips:
            if c is clip or not c[0].endswith('_100'):
                continue
            sp = int(round(BEATS * 60 / 100 * c[1]))
            if c[2] < sp + 8:
                continue
            st = loop_start(bones, c, N, sp, role)
            w = swing(c, st)
            if w < best[0] - 8:
                best = (w, c, st)
        print(name, 'loop', best[1][0], 'guitar swing %.0f deg' % best[0])
        clip, s0 = best[1], best[2]
        span = int(round(BEATS * 60 / 100 * clip[1]))
    M0 = wc.pose(bones, clip, s0)
    g0 = None
    if role in ('guitarist', 'bassist'):
        gi = N['bone_guitar_body']
        g0 = (np.linalg.inv(M0[N['bone_pelvis']]) @ M0[gi], np.linalg.inv(M0[N['bone_chest']]) @ M0[gi])
    anims = load_hands(tex_dir, sex_of(tex_dir, name)) if g0 is not None else None
    frames, centre, rises, stand_y, skins = [], [], [], 0.0, []
    nframes = (SING if role == 'vocalist' else FRAMES) if alt else V_FRAMES
    byname = {c[0]: c for c in clips}
    jp = None
    if role == 'drummer':
        idle = byname.get('idle')
        if idle is not None:
            i_span = 32
            i0 = min(range(0, idle[2] - i_span - 1, 2), key=lambda k: float(np.abs(idle[4][k] - idle[4][k + i_span]).mean()))
        arms = byname.get('armsup_120')
    if role in ('guitarist', 'bassist'):
        jp = byname.get('spjump')
        if jp is not None:
            ys = [wc.pose(bones, jp, k)[N['bone_pelvis']][1, 3] for k in range(jp[2])]
            peak = int(np.argmax(ys))
            j0 = max(0, min(jp[2] - 33, peak - 16))
            print(name, 'spjump peak', peak, 'window', j0, j0 + 32)
    if role == 'vocalist':
        # between lines GHWT's singer has its own idle (ns_100), a loop of its own; Star Power starts with a jump
        ns = byname.get('ns_100', clip)
        ns_span = int(round(BEATS * 60 / 100 * ns[1]))
        ns0 = loop_start(bones, ns, N, ns_span, role)
        jp = byname.get('spjump')
        if jp is not None:
            ys = [wc.pose(bones, jp, k)[N['bone_pelvis']][1, 3] for k in range(jp[2])]
            peak = int(np.argmax(ys))
            j0 = max(0, min(jp[2] - 33, peak - 16))
            print(name, 'spjump peak', peak, 'window', j0, j0 + 32)
    for i in range(nframes):
        f = s0 + i * span // FRAMES
        if role == 'vocalist':
            f = s0 + i * span // SING
        if g0 is not None:
            f = s0 + (i // STRUMS) * span // GROOVE if i < JUMP else s0
        # the drummer keeps one pose (the chart moves the sticks); the guitars groove through the loop
        if role == 'vocalist' and REST <= i < JUMP:
            f = ns0 + (i - REST) * ns_span // (JUMP - REST)
            pose = blended(ns, f, (ns0, ns0 + ns_span))
        elif role == 'drummer' and i >= JUMP:
            if i < JUMP + 8 and idle is not None:
                k = i0 + (i - JUMP) * i_span // 8
                pose = (idle[0], idle[1], 1, idle[3], idle[4][k][None], idle[5][k][None])
            elif arms is not None:
                k = min(arms[2] - 1, (i - JUMP - 8) * (arms[2] - 1) // 7)
                pose = (arms[0], arms[1], 1, arms[3], arms[4][k][None], arms[5][k][None])
            else:
                pose = blended(clip, s0 + span - BLEND, (s0, s0 + span))
        elif i >= JUMP:
            f = j0 + (i - JUMP) * 2 if jp is not None else s0
            # played once, not looped: the frames as they are
            pose = (jp[0], jp[1], 1, jp[3], jp[4][f][None], jp[5][f][None]) if jp is not None else blended(clip, s0, (s0, s0 + span))
        else:
            pose = blended(clip, f if role != 'drummer' else s0 + span - BLEND, (s0, s0 + span))
        if g0 is not None:
            rig = wc.Rig(bones, pose_over(bones, pose, hand_pose(anims, role == 'bassist', sex_of(tex_dir, name) != 'f',
                                                                 GROOVE_SHAPE, STRUM_K[i % STRUMS if i < JUMP else 0])))
        else:
            rig = wc.Rig(bones, wc.pose(bones, pose, 0))
        if role == 'drummer' and i < JUMP:
            for side, pad in STRIKES[i // HIT].items():
                swing_stick(rig, side, pad, HIT_W[i % HIT], HIT_RAISE[i % HIT])
        if role != 'drummer' or 'bone_ik_hand_slave_l' in N:
            wc.solve(rig, rest, role, (f - s0) / span * BEATS, g0)
        if role == 'vocalist':
            straighten(rig, N)
            level_head(rig, N, rest)
            face_front(rig, rest, N)
            hold_mic(rig, i < REST)
        if g0 is not None:
            guitar_hands(rig)
        if role == 'drummer':
            # GHWT's arm IK lands the palm with its target's orientation: the sticks are in the hand (BandAnimator.Solve)
            for side in 'lr':
                pm, tg = N['bone_palm_' + side], N['bone_ik_hand_slave_' + side]
                M = rig.W[pm].copy()
                M[:3, :3] = rig.W[tg][:3, :3]
                rig.set_world(pm, M)
        skin = [m @ b[4] for m, b in zip(rig.W, bones)]
        if os.environ.get('LEANLOG') and role == 'vocalist':
            v = rig.W[N['bone_head']][:3, 3] - rig.W[N['bone_pelvis']][:3, 3]
            print('lean', i, int(math.degrees(math.atan2(math.hypot(v[0], v[2]), v[1]))))
        if role != 'drummer' and i >= JUMP:
            # the jump: the stand isn't in it (it ran up through them), and they're brought down to the floor so the
            # cell holds them; game.c lifts the frame by SINGER_RISE, these numbers
            pel = rig.W[N['bone_pelvis']][:3, 3]
            for bn in ('Bone_Mic_Stand', 'Bone_Mic_Adjust_Height', 'Bone_Mic_Adjust_Angle'):
                if bn in N:
                    Z = np.zeros((4, 4))
                    Z[:3, 3] = pel
                    Z[3, 3] = 1
                    skin[N[bn]] = Z
            if i == JUMP:
                stand_y = float(pel[1])     # from where the jump starts: the same curve for every singer
            rise = max(0.0, float(pel[1] - stand_y))
            rises.append(rise)
            if i == JUMP:
                jump_xz = pel[[0, 2]] - np.mean(centre, axis=0)[[0, 2]]  # the clip jumps from elsewhere on its stage
            for k in range(len(skin)):
                D = np.eye(4)
                D[1, 3] = -rise
                D[0, 3], D[2, 3] = -jump_xz[0], -jump_xz[1]
                skin[k] = D @ skin[k]
        skins.append(skin)
        if not THREED:
            frames.append(skinned(meshes, skin))
        if i < JUMP:
            centre.append(rig.W[N['bone_pelvis']][:3, 3])

    if role == 'vocalist' and 'Bone_Mic_Stand' in N and skins:
        # the mic stand where the singer stands: the loops each leave it somewhere of their own (up to a metre off), and
        # the game brings every frame's singer back over the frames' middle (band3d.c, by bone 1's place) while the stand
        # stays in the loop's spot. So it's put a step in front of that middle in every frame, the same each time
        # (band3d.c then leaves bones that never move out of its re-centring, and gives the wilder loop this stand)
        mid = np.mean([sk[1][:3, 3] for sk in skins], axis=0)
        sb = [N[b] for b in ('Bone_Mic_Stand', 'Bone_Mic_Adjust_Height', 'Bone_Mic_Adjust_Angle') if b in N]
        joint = np.r_[rest[N['Bone_Mic_Stand']][:3, 3], 1.0]
        for f, sk in enumerate(skins):
            if alt is False and f >= JUMP:
                continue                    # the jump hides it
            at = (sk[N['Bone_Mic_Stand']] @ joint)[:3]
            if np.abs(sk[N['Bone_Mic_Stand']][:3, :3]).sum() < 1e-6:
                continue
            T = np.eye(4)
            T[0, 3] = mid[0] - at[0]
            T[2, 3] = (mid[2] - STAND_AHEAD) - at[2]
            for b in sb:
                sk[b] = T @ sk[b]
    c = np.mean(centre, axis=0) if centre else np.zeros(3)
    if THREED:
        layers = None
        if g0 is not None:
            # the fret hand apart from the body: every groove step with the hand at each of LAYER_FRETS in each of
            # LAYER_CHORDS; the bones that differ between them are the layer's (the arm, the hand, the fingers)
            male = sex_of(tex_dir, name) != 'f'
            all_ = np.zeros((GROOVE, len(LAYER_FRETS), len(LAYER_CHORDS), len(bones), 3, 4))
            for g in range(GROOVE):
                f = s0 + g * span // GROOVE
                pose = blended(clip, f, (s0, s0 + span))
                for pi, fret in enumerate(LAYER_FRETS):
                    for ci, chord in enumerate(LAYER_CHORDS):
                        rig = wc.Rig(bones, pose_over(bones, pose, hand_pose(anims, role == 'bassist', male, 0,
                                                                             STRUM_K[0], chord, fret)))
                        wc.solve(rig, rest, role, (f - s0) / span * BEATS, g0)
                        guitar_hands(rig)
                        all_[g, pi, ci] = np.stack([(m @ b[4])[:3, :] for m, b in zip(rig.W, bones)])
            spread = np.abs(all_ - all_[:, :1, :1]).max(axis=(0, 1, 2, 4, 5))
            lb = np.nonzero(spread > 1e-4)[0]
            layers = (lb.astype('<i4'), all_[:, :, :, lb])
            # the fingers' 16 poses, each bone's skin matrix as the palm's times this
            low = {b[0].lower(): i for i, b in enumerate(bones)}
            palm = low['bone_palm_l']
            fb = [i for i, b in enumerate(bones) if b[0].lower().startswith('bone_hand_') and b[0].lower().endswith('_l')]
            C = np.zeros((16, len(fb), 3, 4))
            pose = blended(clip, s0, (s0, s0 + span))
            for mask in range(16):
                over = hand_pose(anims, role == 'bassist', male, 0, STRUM_K[0], 'empty', LAYER_FRETS[0])
                over.update(finger_pose(anims, male, mask))
                rig = wc.Rig(bones, pose_over(bones, pose, over))
                wc.solve(rig, rest, role, 0, g0)
                guitar_hands(rig)
                press_fingers(rig, mask)
                S = [m @ b[4] for m, b in zip(rig.W, bones)]
                ip = np.linalg.inv(S[palm])
                for k, i in enumerate(fb):
                    C[mask, k] = (ip @ S[i])[:3, :]
            FINGER_TABLE[name] = (palm, np.array(fb, '<i4'), C.astype('<f4'))
        export3d(name, meshes, skins, c, alt, tex_dir, layers, bones, work)
        return
    binp = os.path.join(work, name + '.bin')
    ca, sa = math.cos(PITCH), math.sin(PITCH)
    with open(binp, 'wb') as o:
        o.write(struct.pack('<ii', len(meshes), nframes))
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
    sheet = Image.new('RGBA', (CW * COLS, CH * V_ROWS), (0, 0, 0, 0))
    for i in range(nframes):
        im = Image.open('%s_%02d.png' % (pre, i)).convert('RGBa').resize((CW, CH), Image.BOX).convert('RGBA')
        sheet.paste(im, ((i % COLS) * CW, (i // COLS) * CH))
    out = os.path.join(OUT, 'alt', name + '.png') if alt else os.path.join(OUT, name + '.png')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    if alt:
        sheet = sheet.crop((0, 0, CW * COLS, CH * ((nframes + COLS - 1) // COLS)))
    sheet.quantize(256, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE).save(out, optimize=True)
    if rises:
        # game.c lifts the jump's frames back up by these (assets/band/rise.txt: name and 16 heights in metres)
        rp = os.path.join(OUT, 'rise.txt')
        old = [l for l in open(rp).read().splitlines() if l.split(' ')[0] != name] if os.path.exists(rp) else []
        with open(rp, 'w') as rf:
            rf.write('\n'.join(sorted(old + [name + ' ' + ' '.join('%.3f' % r for r in rises)])) + '\n')
        print(name, 'rise', ' '.join('%.3f' % r for r in rises))
    print(name, clip[0], 'from frame', s0, 'span', span, os.path.getsize(out) // 1024, 'KB')


def main():
    global THREED
    alt = '--alt' in sys.argv
    THREED = '--3d' in sys.argv
    args = [a for a in sys.argv[1:] if a not in ('--alt', '--3d')]
    tex_dir, work = args[0], args[1]
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(work, exist_ok=True)
    picks = args[2:]
    for p in sorted(glob.glob(os.path.join(tex_dir, 'band_*.wtch'))):
        if not picks or os.path.basename(p)[5:-5] in picks:
            bake(p, work, tex_dir, alt)
    if not picks and not alt and not THREED:
        with open(os.path.join(OUT, 'band.txt'), 'w') as f:
            f.write('\n'.join(sorted(x[:-4] for x in os.listdir(OUT) if x.endswith('.png'))) + '\n')


if __name__ == '__main__':
    main()
