#!/usr/bin/env python3
"""Puts the build at HEAD on GitHub, where Vitas with update.txt pick it up: release.py [--notes "what changed"]
Builds the VPK from a clean tree, tags v<commit count>, and uploads:
  GHVita.vpk               the whole thing, for a first install
  GHVitaUpdater.vpk        the little app that swaps updates in; install it once, alongside the game
  update-full.zip          every file the game has, for a Vita more than one release behind
  update-from-<last>.zip   only what changed since the last release
  manifest.txt             what the next release compares itself with
The VPK holds art made from your own games: the repository has to stay private."""
import argparse, json, os, shutil, subprocess, sys, tempfile, zipfile, zlib

ap = argparse.ArgumentParser(); ap.add_argument('--notes', default=''); ap.add_argument('--dry', action='store_true')
a = ap.parse_args()
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__))); os.chdir(root)
run = lambda *c, **k: subprocess.run(c, check=True, text=True, capture_output=True, **k).stdout.strip()
if json.loads(run('gh', 'repo', 'view', '--json', 'isPrivate'))['isPrivate'] is not True:
    sys.exit('the repository is public: the release would publish the art')
if run('git', 'status', '--porcelain'):
    sys.exit('commit first: a release is exactly what HEAD is')
n = int(run('git', 'rev-list', '--count', 'HEAD')); tag = f'v{n}'
if subprocess.run(['gh', 'release', 'view', tag], capture_output=True).returncode == 0:
    sys.exit(f'{tag} is out already')
sdk = os.environ.get('VITASDK', os.path.expanduser('~/opt/vitasdk'))
env = dict(os.environ, VITASDK=sdk, PATH=sdk + '/bin:' + os.environ['PATH'])
ninja = shutil.which('ninja', path=env['PATH'])
if not ninja:
    sys.exit('ninja not found on the path: install it (apt install ninja-build, or pip install ninja)')
# the release builds in its own tree, so a test build's settings never leak into it
subprocess.run(['cmake', '-S', '.', '-B', 'build-release', '-G', 'Ninja', f'-DCMAKE_TOOLCHAIN_FILE={sdk}/share/vita.toolchain.cmake', f'-DCMAKE_MAKE_PROGRAM={ninja}', f'-DGHV_BUILD={n}'], check=True, env=env, stdout=subprocess.DEVNULL)
subprocess.run([ninja, '-C', 'build-release'], check=True, env=env, stdout=subprocess.DEVNULL)
vpk = 'build-release/GHVita.vpk'
updater_vpk = 'build-release/GHVitaUpdater.vpk'
tmp = tempfile.mkdtemp()
z = zipfile.ZipFile(vpk); z.extractall(tmp)
files = {}
for d, _, fs in os.walk(tmp):
    for f in fs:
        p = os.path.relpath(os.path.join(d, f), tmp)
        if p in ('eboot.bin', 'updater.bin') or p.startswith('assets/'):
            b = open(os.path.join(d, f), 'rb').read()
            files[p] = (len(b), zlib.crc32(b))
line = lambda p: '%d %08x %s\n' % (files[p][0], files[p][1], p)
def pack(path, names):
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as o:
        o.writestr('manifest.txt', ''.join(line(p) for p in sorted(names)))
        for p in sorted(names):
            o.write(os.path.join(tmp, p), p)
out = tempfile.mkdtemp()
pack(out + '/update-full.zip', files)
if not os.path.exists(updater_vpk):
    sys.exit('the build made no updater VPK')
up = [vpk, updater_vpk, out + '/update-full.zip', out + '/manifest.txt']
open(out + '/manifest.txt', 'w').write(''.join(line(p) for p in sorted(files)))
last = subprocess.run(['gh', 'release', 'view', '--json', 'tagName', '-q', '.tagName'], capture_output=True, text=True).stdout.strip()
if last:
    d = tempfile.mkdtemp()
    subprocess.run(['gh', 'release', 'download', last, '-p', 'manifest.txt', '-D', d], check=True)
    had = {}
    for l in open(d + '/manifest.txt'):
        s, c, p = l.rstrip('\n').split(' ', 2); had[p] = (int(s), int(c, 16))
    changed = [p for p in files if had.get(p) != files[p]]
    print(f'{len(changed)} of {len(files)} files changed since {last}')
    if changed:
        pack(f'{out}/update-from-{last[1:]}.zip', changed); up.append(f'{out}/update-from-{last[1:]}.zip')
for p in up:
    print('%8.1f MB %s' % (os.path.getsize(p) / 1048576, os.path.basename(p)))
if a.dry:
    sys.exit(print('dry run: nothing uploaded', tag))
notes = a.notes or run('git', 'log', '-1', '--format=%s')
subprocess.run(['gh', 'release', 'create', tag, '--title', f'GH Vita build {n}', '--notes', notes, '--latest', *up], check=True)
print('released', tag)
