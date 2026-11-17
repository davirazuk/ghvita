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
ap.add_argument('--no-smoke', action='store_true', help='skip test/smoke.sh')
a = ap.parse_args()
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__))); os.chdir(root)
run = lambda *c, **k: subprocess.run(c, check=True, text=True, capture_output=True, **k).stdout.strip()
if json.loads(run('gh', 'repo', 'view', '--json', 'isPrivate'))['isPrivate'] is not True:
    sys.exit('the repository is public: the release would publish the art')
if run('git', 'status', '--porcelain'):
    sys.exit('commit first: a release is exactly what HEAD is')
n = int(run('git', 'rev-list', '--count', 'HEAD')); tag = f'v{n}'
# the bot plays the test songs on the desktop build first: a build that crashes or drops notes never goes out
if not a.no_smoke and subprocess.run(['bash', 'test/smoke.sh']).returncode != 0:
    sys.exit('the smoke test failed (test/smoke.sh): fix it, or --no-smoke to release anyway')
# the build number also has to pass the newest release (a squash or a merge can leave the count behind it), or the
# Vita's updater would never offer it
newest = subprocess.run(['gh', 'release', 'view', '--json', 'tagName', '-q', '.tagName'], capture_output=True, text=True).stdout.strip()
if newest[1:].isdigit() and int(newest[1:]) >= n:
    n = int(newest[1:]) + 1; tag = f'v{n}'
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
# the Updater app on Vitas out there moves 1024 files and no more (update_apply.c MAX_FILES as it was): past that it
# stopped short, the program never went in, and the update was offered again for ever
if len(files) > 1000:
    sys.exit(f'{len(files)} files: more than the installed Updater app can move (1024); pack some together')
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
# GitHub has refused this now and then (HTTP 403 "Resource not accessible by integration", everything else done): the
# release made empty first, the files then, with a retry each; a half-made release is cleared before the next try
import time
head = run('git', 'rev-parse', 'HEAD')
for attempt in range(4):
    made = subprocess.run(['gh', 'release', 'create', tag, '--target', head, '--title', f'GH Vita build {n}', '--notes', notes, '--latest'],
                          capture_output=True, text=True)
    if made.returncode == 0:
        up_ok = subprocess.run(['gh', 'release', 'upload', tag, '--clobber', *up], capture_output=True, text=True)
        if up_ok.returncode == 0:
            break
        print('upload failed:', up_ok.stderr.strip())
    else:
        print('create failed:', made.stderr.strip())
    subprocess.run(['gh', 'release', 'delete', tag, '--yes', '--cleanup-tag'], capture_output=True)
    time.sleep(30 * (attempt + 1))
else:
    sys.exit('the release could not be made')
print('released', tag)
