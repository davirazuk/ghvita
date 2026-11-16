#!/usr/bin/env python3
"""Mends the 3D band's loose bones in assets/band3d, in place: band_bones.py [assets/band3d]
World Tour moves a member's hair, hat, beard, coat tails and the like itself while the game runs: their bones hang off
the hips in the files, and the loops tools/band.py baked left them there, so a head that bows left its hair and its
face's pieces behind (spikes out of the head, a face torn open). Here each such bone is fixed to the body's bone its
own piece of the mesh grows from (hair to the head, a tail to the spine), in every frame of both loops.
A mended file has no such bones left, so running it again changes nothing."""
import glob, os, struct, sys
import numpy as np

HIPS = 1        # the bone the loose ones were left on (a twist bone that copies its limb is as it should be)
VD = np.dtype([('p', '<f4', 3), ('n', '<f4', 3), ('uv', '<f4', 2), ('j', 'u1', 4), ('w', 'u1', 4)])


def parts_of(d, at, n):
    out = []
    for _ in range(n):
        vc, tc, _ = struct.unpack('<3i', d[at + 32:at + 44])
        at += 44
        v = np.frombuffer(d, VD, vc, at)
        at += vc * 40
        out.append((v, np.frombuffer(d, '<u2', tc * 3, at).reshape(-1, 3)))
        at += tc * 6
    return out, at


def mend(path):
    d = bytearray(open(path, 'rb').read())
    if d[:4] != b'B3D2':
        return None
    head = struct.unpack('<7i', d[4:32])
    parts, at = parts_of(d, 32, head[0])
    nb, nf = head[1], head[2]
    M = np.frombuffer(d, '<f4', nf * nb * 12, at).reshape(nf, nb, 12).copy()
    # what each bone's vertices touch: the other bone of a vertex, and the bones across its triangles' edges
    touch = np.zeros((nb, nb))
    for v, idx in parts:
        j, w = v['j'][:, :2].astype(int), v['w'][:, :2].astype(float)
        both = w[:, 1] > 0
        np.add.at(touch, (j[both, 0], j[both, 1]), 2)
        np.add.at(touch, (j[both, 1], j[both, 0]), 2)
        main = j[:, 0]
        # vertices at one place are one vertex here (the mesh is cut along its texture's seams)
        _, weld = np.unique(np.round(v['p'] * 2000).astype(np.int64), axis=0, return_inverse=True)
        weld = weld.ravel()
        rep = np.zeros(weld.max() + 1, int)
        rep[weld] = main
        for a, b in ((0, 1), (1, 2), (2, 0)):
            x, y = main[idx[:, a]], main[idx[:, b]]
            np.add.at(touch, (x, y), 1)
            np.add.at(touch, (y, x), 1)
            x, y = main[idx[:, a]], rep[weld[idx[:, a]]]
            np.add.at(touch, (x, y), 1)
            np.add.at(touch, (y, x), 1)
    np.fill_diagonal(touch, 0)
    moves = np.array([np.abs(M[:, b] - M[0, b]).max() > 1e-4 for b in range(nb)])
    # how far a bone's piece of the mesh is pulled out of shape over the loop, the bone moving as `as_bone` does
    P4 = [np.concatenate([v['p'], np.ones((len(v), 1), np.float32)], 1) for v, _ in parts]
    fr = np.arange(0, nf, max(1, nf // 16))

    def strain(b, as_bone):
        total = 0.0
        for (v, idx), p4 in zip(parts, P4):
            j, w = v['j'][:, :2].astype(int), v['w'][:, :2].astype(np.float32)
            on = (j[:, 0] == b) | ((j[:, 1] == b) & (w[:, 1] > 0))
            tri = idx[on[idx].any(1)]
            if len(tri) == 0:
                continue
            used = np.unique(tri)
            Mb = M[fr].copy()
            Mb[:, b] = M[fr][:, as_bone]
            ws = w[used] / np.maximum(w[used].sum(1, keepdims=True), 1)
            pos = np.zeros((len(fr), len(used), 3), np.float32)
            for k in range(2):
                pos += np.einsum('fnij,nj->fni', Mb[:, j[used, k]].reshape(len(fr), len(used), 3, 4), p4[used]) * ws[None, :, k:k + 1]
            at = np.searchsorted(used, tri)
            for x, y in ((0, 1), (1, 2), (2, 0)):
                rest = np.linalg.norm(v['p'][tri[:, x]] - v['p'][tri[:, y]], axis=1)
                now = np.linalg.norm(pos[:, at[:, x]] - pos[:, at[:, y]], axis=2)
                total += float(np.abs(now - rest[None]).mean(0).sum())
        return total

    # The loose bones are the ones left the same as the hips in every frame. Bones that are the same are one group, and
    # a loose bone whose piece of the mesh grows more from another group than from its own joins that one, the surest
    # first, until none is left (so a strand of hair follows its root to the head, link by link). Then any that's
    # pulled further out of shape there than it was on the hips goes back (a skirt's hem on a seated drummer's thigh)
    to = {}
    while True:
        group = np.arange(nb)       # the first bone each is the same as (to a rounding's width)
        for b in range(nb):
            for q in range(b):
                if group[q] == q and np.abs(M[:, b] - M[:, q]).max() < 1e-3:
                    group[b] = q
                    break
        best = None
        for b in range(nb):
            if not moves[b] or b == HIPS or group[b] != group[HIPS] or touch[b].sum() == 0:
                continue
            t = np.zeros(nb)
            np.add.at(t, group, touch[b])
            own = t[group[b]]
            t[group[b]] = 0
            a = int(t.argmax())
            if t[a] > own and (best is None or t[a] - own > best[2]):
                best = (b, a, t[a] - own)
        if best is None:
            break
        M[:, best[0]] = M[:, best[1]]
        to[best[0]] = best[1]
    for b in sorted(to):
        if strain(b, HIPS) < 0.9 * strain(b, b):
            M[:, b] = M[:, HIPS]
            del to[b]
    if not to:
        return {}
    d[at:at + nf * nb * 48] = M.astype('<f4').tobytes()
    open(path, 'wb').write(d)
    alt = os.path.join(os.path.dirname(path), 'alt', os.path.basename(path)[:-4] + '.b3a')
    if os.path.exists(alt):
        a = bytearray(open(alt, 'rb').read())
        h = struct.unpack('<6i', a[4:28])
        if a[:4] == b'B3A2' and h[0] == nb:
            A = np.frombuffer(a, '<f4', h[1] * nb * 12, 28).reshape(h[1], nb, 12).copy()
            for b, t in to.items():
                A[:, b] = A[:, t]
            a[28:28 + h[1] * nb * 48] = A.astype('<f4').tobytes()
            open(alt, 'wb').write(a)
    return to


if __name__ == '__main__':
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'assets', 'band3d')
    for f in sorted(glob.glob(os.path.join(root, '*.b3d'))):
        to = mend(f)
        print(os.path.basename(f), 'not a model' if to is None else ' '.join('%d>%d' % kv for kv in sorted(to.items())) or 'nothing loose')
