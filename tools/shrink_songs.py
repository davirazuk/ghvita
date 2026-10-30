#!/usr/bin/env python3
# shrinks a songs folder (the card's) in place, with the originals in backup/: each stem to stereo Opus. The decoder
# goes by what's in a file, not its name, so a song's .ogg names and ghvita.ini stay. A Rock Band song.ogg of 12+
# tracks is split by its song.mix into a stem per instrument (the rest, crowd too, stays song.ogg), which a miss mutes
import glob, json, os, shutil, subprocess, sys

ONE_STEM, STEM = "128k", "96k"


def probe(path):
    r = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "stream=codec_name,channels:format=duration",
                        "-of", "json", path], capture_output=True, text=True, check=True)
    d = json.loads(r.stdout)
    return d["streams"][0]["codec_name"], d["streams"][0]["channels"], float(d["format"]["duration"])


RB_STEM = "56k"


def read_mix(folder, channels):
    gains, parts = [], {}
    for line in open(os.path.join(folder, "song.mix")):
        w = line.split()
        if w[0] == "part":
            parts[w[1]] = [int(c) for c in w[2:] if int(c) < channels]
        else:
            gains.append((int(w[0]) / 4096, int(w[1]) / 4096))
    return gains[:channels], parts


def pan(gains, tracks):
    left = "+".join(f"{gains[c][0]:.4f}*c{c}" for c in tracks)
    right = "+".join(f"{gains[c][1]:.4f}*c{c}" for c in tracks)
    return f"pan=stereo|c0={left}|c1={right},alimiter=limit=0.98:level=disabled"


def split_rock_band(folder, path, channels):
    gains, parts = read_mix(folder, channels)
    used = {c for t in parts.values() for c in t}
    parts["song"] = [c for c in range(channels) if c not in used]
    made = []
    for name, tracks in parts.items():
        if not tracks:
            continue
        out = os.path.join(folder, name + ".ogg.tmp")
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", path, "-vn", "-af", pan(gains, tracks), "-ac", "2",
                        "-ar", "48000", "-c:a", "libopus", "-b:a", RB_STEM, "-f", "ogg", out], check=True)
        made.append(out)
    return made


songs, backup = sys.argv[1], sys.argv[2]
before = after = 0
for folder in sorted(d for d in glob.glob(os.path.join(glob.escape(songs), "*")) if os.path.isdir(d)):
    stems = sorted(glob.glob(os.path.join(glob.escape(folder), "*.ogg")))
    for path in stems:
        codec, channels, length = probe(path)
        before += os.path.getsize(path)
        if codec == "opus":
            after += os.path.getsize(path)
            continue
        if channels > 2:
            keep = os.path.join(backup, os.path.basename(folder))
            os.makedirs(keep, exist_ok=True)
            made = split_rock_band(folder, path, channels)
            if any(abs(probe(m)[2] - length) > 0.1 for m in made):
                sys.exit("a stem came out the wrong length: " + path)
            shutil.copy2(path, keep)
            shutil.copy2(os.path.join(folder, "song.mix"), keep)
            os.remove(os.path.join(folder, "song.mix"))
            os.remove(path)
            for m in made:
                os.replace(m, m[:-4])
                after += os.path.getsize(m[:-4])
            print(f"{os.path.basename(folder)}: {channels} tracks to {len(made)} stems")
            continue
        args = ["ffmpeg", "-v", "error", "-y", "-i", path, "-vn"]
        tmp = path + ".tmp"
        args += ["-ac", "2", "-ar", "48000", "-c:a", "libopus", "-b:a", ONE_STEM if len(stems) == 1 else STEM,
                 "-f", "ogg", tmp]
        subprocess.run(args, check=True)
        code2, ch2, len2 = probe(tmp)
        if code2 != "opus" or ch2 != 2 or abs(len2 - length) > 0.1 or os.path.getsize(tmp) >= os.path.getsize(path):
            os.remove(tmp)
            print("kept", path)
            after += os.path.getsize(path)
            continue
        keep = os.path.join(backup, os.path.basename(folder))
        os.makedirs(keep, exist_ok=True)
        shutil.copy2(path, keep)
        os.replace(tmp, path)
        after += os.path.getsize(path)
        print(f"{os.path.basename(folder)}/{os.path.basename(path)}: {channels} ch, {length:.0f} s")
print(f"{before / 1048576:.1f} MB to {after / 1048576:.1f} MB")
