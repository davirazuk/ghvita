# GH Vita

A native rhythm game for the PS Vita in the style of Guitar Hero World Tour, written in C on VitaSDK and vitaGL. It plays Clone Hero and YARG songs, and can find and download them on the Vita itself.

## What it does

- Guitar, bass, keys, drums and vocals, with singing along over any instrument
- Star Power, solos, sustains and a whammy that bends the part's stem, on the right stick or the rear touchpad
- Drums with kicks, two button layouts, forgiving rolls and freestyle in the gaps of the chart
- Practice: any stretch of a song by its sections, looping, from 50% to full speed
- A downloader for Chorus Encore and RhythmVerse, with Clone Hero folders, `.sng` packages and unencrypted Rock Band packages; downloads are turned into small Opus stems on the Vita
- A venue that follows the song: lights by section and loudness, moving beams, a crowd that jumps with how you're doing, screens with the cover, a camera that closes in on the band, and a band that plays its own parts of the chart

## What isn't here

This repository holds the code only. The art and sound in `assets/` and the LiveArea images in `vita/sce_sys/` are made from your own copies of the games, and songs are yours to bring. `tools/mkassets.py`, `tools/venues.py`, `tools/band.py` and `tools/band_ink.py` build them, `tools/mkstage.py` draws the stage's rig and crowd, and `tools/mkdrums.py` cuts the drum kit from a YARG install.

## Building

For the Vita, with VitaSDK installed and these packages from vdpm: curl, libarchive, un7zip, opusfile, opus, mpg123, bzip2, zstd.

```
cmake -S . -B build-vita -G Ninja -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
ninja -C build-vita
```

That makes `build-vita/GHVita.vpk`.

The desktop build runs the same game on SDL2 and OpenGL, for testing:

```
cmake -S . -B build-pc -G Ninja
ninja -C build-pc
cd test && GHV_DATA=data GHV_ASSETS=../assets ../build-pc/ghvita
```

`PLAN.md` has the file formats, the test harness and how each part works.
