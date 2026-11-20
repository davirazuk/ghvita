# Blender (headless) side of tools/band.py --3d: thins each mesh of a member with the Decimate modifier, which carries
# the texture places and the bone weights through the collapse (a nearest-vertex copy smears faces across seams).
#   blender -b --python band_decimate_bl.py -- in.npz out.npz
# in: count, and per mesh i: p (n x 3), uv (n x 2), j (n x 4 bone ids), w (n x 4), a (n), t (m x 3), r (ratio)
# out: per mesh i: p, n, uv, j (k x 2), w (k x 2), a, t
import sys
import bpy, bmesh
import numpy as np

argv = sys.argv[sys.argv.index('--') + 1:]
d = np.load(argv[0])
out = {'count': d['count']}
for i in range(int(d['count'])):
    P, UV, J, W, A, T = (d['%s%d' % (k, i)] for k in ('p', 'uv', 'j', 'w', 'a', 't'))
    ratio = float(d['r%d' % i])
    me = bpy.data.meshes.new('m%d' % i)
    me.from_pydata(P.tolist(), [], T.tolist())
    me.update()
    lv = np.zeros(len(me.loops), np.int32)
    me.loops.foreach_get('vertex_index', lv)
    uvl = me.uv_layers.new(name='uv')
    uvl.data.foreach_set('uv', UV[lv].astype(np.float32).ravel())
    ob = bpy.data.objects.new('o%d' % i, me)
    bpy.context.collection.objects.link(ob)
    groups = {}
    for v in range(len(P)):
        for k in range(4):
            if W[v, k] > 0.004:
                b = int(J[v, k])
                g = groups.get(b)
                if g is None:
                    g = groups[b] = ob.vertex_groups.new(name=str(b))
                g.add([v], float(W[v, k]), 'ADD')
    has_a = bool((A < 0.999).any())
    if has_a:
        ga = ob.vertex_groups.new(name='A')
        for v in range(len(P)):
            ga.add([v], float(max(A[v], 0.0)), 'REPLACE')
    bm = bmesh.new()
    bm.from_mesh(me)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-5)
    bm.to_mesh(me)
    bm.free()
    if ratio < 0.999:
        mod = ob.modifiers.new('d', 'DECIMATE')
        mod.ratio = ratio
        mod.use_collapse_triangulate = True
        # the hands protected (band.py: k, each vertex's share of a hand): the collapse takes the rest
        K = d['k%d' % i] if ('k%d' % i) in d.files else None
        if K is not None and float(K.max()) > 0.05:
            gk = ob.vertex_groups.new(name='K')
            for v in range(len(P)):
                if K[v] > 0.05:
                    gk.add([v], float(min(K[v], 1.0)), 'REPLACE')
            mod.vertex_group = 'K'
            mod.invert_vertex_group = True
            mod.vertex_group_factor = 1000.0
    ev = ob.evaluated_get(bpy.context.evaluated_depsgraph_get())
    m2 = ev.to_mesh()
    m2.calc_loop_triangles()
    names = [g.name for g in ob.vertex_groups]
    uvd = m2.uv_layers[0].data
    index, p, n, uv, jj, ww, aa, tris = {}, [], [], [], [], [], [], []
    for lt in m2.loop_triangles:
        tri = []
        for li in lt.loops:
            vi = m2.loops[li].vertex_index
            u = uvd[li].uv
            key = (vi, round(u[0], 5), round(u[1], 5))
            k = index.get(key)
            if k is None:
                k = index[key] = len(p)
                v = m2.vertices[vi]
                p.append(tuple(v.co))
                n.append(tuple(v.normal))
                uv.append((u[0], u[1]))
                ws, alpha = [], 1.0
                for g in v.groups:
                    nm = names[g.group]
                    if nm == 'A':
                        alpha = g.weight
                    elif nm == 'K':
                        continue            # (the hands' protection, not a bone)
                    elif g.weight > 0:
                        ws.append((g.weight, int(nm)))
                ws.sort(reverse=True)
                ws = (ws + [(0.0, 0), (0.0, 0)])[:2]
                tot = ws[0][0] + ws[1][0]
                if tot <= 0:
                    ws, tot = [(1.0, int(J[0, 0])), (0.0, 0)], 1.0
                jj.append((ws[0][1], ws[1][1]))
                ww.append((ws[0][0] / tot, ws[1][0] / tot))
                aa.append(alpha if has_a else 1.0)
            tri.append(k)
        if tri[0] != tri[1] and tri[1] != tri[2] and tri[0] != tri[2]:
            tris.append(tri)
    ev.to_mesh_clear()
    out['p%d' % i] = np.array(p, np.float32).reshape(-1, 3)
    out['n%d' % i] = np.array(n, np.float32).reshape(-1, 3)
    out['uv%d' % i] = np.array(uv, np.float32).reshape(-1, 2)
    out['j%d' % i] = np.array(jj, np.int32).reshape(-1, 2)
    out['w%d' % i] = np.array(ww, np.float32).reshape(-1, 2)
    out['a%d' % i] = np.array(aa, np.float32)
    out['t%d' % i] = np.array(tris, np.int32).reshape(-1, 3)
np.savez(argv[1], **out)
