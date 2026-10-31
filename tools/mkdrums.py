# the kit drums play in the gaps of a chart and, if asked, on every hit: assets/sfx/drum_*.ogg, cut from the sampled kit
# in a YARG install: mkdrums.py [YARG_Data/StreamingAssets/drumSfx]
import os, subprocess, sys

KIT = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser(
    '~/mods/yarg-look/testinst/installation/YARG_Data/StreamingAssets/drumSfx')
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'assets', 'sfx')
HARD = 2        # YARG's velocity: 0 ghost, 1 normal, 2 accent

# name, YARG's pad, takes, seconds kept (a cymbal rings much longer than anyone hears over a song), peak
PIECES = [
    ('kick', 7, 3, 0.5, 0.95), ('snare', 0, 3, 0.6, 0.85), ('hat', 4, 3, 0.4, 0.5),
    ('tom_hi', 1, 2, 0.8, 0.8), ('tom_mid', 2, 2, 0.8, 0.8), ('tom_lo', 3, 2, 0.9, 0.85),
    ('ride', 5, 2, 1.5, 0.5), ('crash', 6, 2, 2.6, 0.65),
]


def peak(path):
    out = subprocess.run(['ffmpeg', '-v', 'info', '-i', path, '-t', '1', '-af', 'volumedetect', '-f', 'null', '-'],
                         capture_output=True, text=True).stderr
    return float(out.split('max_volume:')[1].split()[0])        # dB


for name, pad, takes, secs, want in PIECES:
    files = [os.path.join(KIT, 'vel%dpad%dsmp%d.opus' % (HARD, pad, k)) for k in range(takes)]
    top = max(peak(f) for f in files)
    gain = 20 * __import__('math').log10(want) - top            # the takes keep their levels against each other
    for k, f in enumerate(files):
        fade = secs * 0.55
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', f, '-t', str(secs), '-af',
                        'volume=%.2fdB,afade=t=out:st=%.3f:d=%.3f' % (gain, secs - fade, fade), '-ar', '48000', '-ac',
                        '2', '-c:a', 'libvorbis', '-q:a', '3', os.path.join(OUT, 'drum_%s_%d.ogg' % (name, k + 1))],
                       check=True)
    print(name, 'gain %+.1f dB' % gain)
