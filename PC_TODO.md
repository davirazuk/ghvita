# For Claude on Davi's PC

Written by the Claude working on this repo in the cloud, for the Claude that Davi runs on his PC with the PS Vita
plugged in. Do both jobs, in order, then **delete this file** (last section). Ask Davi before anything that isn't here.

## 1. Put the newest GH Vita on the Vita

Builds 62 to 72 were linked with an OpenSSL curl that can't connect on the Vita: no song downloads and no update checks.
The Vita is on build 68, so it can't fetch the fix itself. Install it by hand, once.

1. Download `GHVita.vpk` from the newest release of the private repo `davirazuk/ghvita`:
   `gh release download --repo davirazuk/ghvita --pattern GHVita.vpk --dir .`
   Make sure the release is **73 or newer** and its notes say *Downloads and updates work on the Vita again*. If the
   newest is still 72 or older, the fixed build isn't out yet: look at the repo's Actions tab, wait, and don't install
   an older one.
2. Davi opens VitaShell on the Vita and presses SELECT for USB mode. The memory card (`ux0:`) shows up as a drive.
3. Copy `GHVita.vpk` to the root of that drive (`ux0:/GHVita.vpk`), then eject the drive.
4. Davi presses Circle to leave USB mode, highlights `ux0:/GHVita.vpk` in VitaShell, presses X and confirms the install.
   It goes over the old game. Songs, players, scores and settings live in `ux0:data/ghvita/` and are untouched; don't
   touch that folder.
5. Check: GH Vita opens, and DOWNLOAD SONGS lists Chorus Encore charts. Afterwards the VPK can be deleted from `ux0:/`.

## 2. Give the cloud Claude the World Tour source files

The art in `assets/` (and `vita/sce_sys/`) is made by the tools in `tools/` from Davi's own copies of the games. The
cloud Claude only has the finished `assets/` (taken out of a release VPK), so it can't remake or change any of it. It
needs what those tools read. Each tool's header and `PLAN.md` say what; the paths on this PC are probably:

- **YargLook's decoded GHWT textures**: `~/mods/yarg-look/YargLook/textures` (venues `venue_ghwt_<zone>.wtch` + `.json`,
  band members `band_<role>*.wtch`, and the GHWT art `tools/mkassets.py` reads). See `tools/venues.py`, `tools/band.py`
  and `tools/mkassets.py` for the exact inputs.
- **ghwt-tools**: `wtch_check.py` (and what it imports), which `tools/band.py` uses.
- **YARG's drum samples**: `YARG_Data/StreamingAssets/drumSfx` from a YARG install (`tools/mkdrums.py`), and YARG's
  sound effects that `tools/mkassets.py` uses.

Read the tools to confirm each input's real location on this PC. Then:

1. Pack them into one archive per source, keeping the folder layout (for example `ghwt-textures.7z`, `ghwt-tools.7z`,
   `yarg-sfx.7z`), each under 2 GB.
2. Upload them as a **pre-release** named `source-art`, so the Vita's updater never takes it for a game build:
   `gh release create source-art --repo davirazuk/ghvita --prerelease --title "Source art (not a build)" --notes "World Tour and YARG files the asset tools read. Not a game build." ghwt-textures.7z ghwt-tools.7z yarg-sfx.7z`
   (If `source-art` already exists: `gh release upload source-art ... --clobber`.)
3. **Never commit these files to the repository.** They're the games' own files: they stay out of git history, and the
   repository and its releases stay private.

Then tell Davi it's uploaded, with the file list and sizes, so he can tell the cloud Claude.

## 3. Remove this file

When both jobs are done (or if Davi says to drop them), delete `PC_TODO.md`, commit with a short message saying it's
done, and push to `main`. Only this file: nothing else in the repo.

A note for the cloud Claude: if this file is still here in a later session, the PC jobs may not have happened yet.
Ask Davi, and remove it once he says they're done.
