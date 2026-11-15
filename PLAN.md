# GH Vita: a Guitar Hero World Tour-style game for the PS Vita

Native C (VitaSDK + vitaGL fixed-function GL, SceAudioOut), plus a desktop SDL2/OpenGL 2.1 test build of the same code.
Plays Clone Hero/YARG songs: downloaded on the Vita itself (DOWNLOAD SONGS), copied raw to songs/ (folders or .sng,
converted at the next start), or converted on the PC by tools/pack.py. Title ID GHVT00001, data ux0:data/ghvita/.

## Build and test
- Vita: `cmake -S . -B build-vita -G Ninja -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake && ninja -C build-vita`
  -> build-vita/GHVita.vpk (assets/ and vita/sce_sys/ go in it).
- PC: `cmake -S . -B build-pc -G Ninja && ninja -C build-pc`; run from test/ with GHV_DATA=data GHV_ASSETS=../assets.
  The PC build links the system's .so libraries (curl, libarchive, opusfile, mpg123, vorbis); their headers come from
  -dev packages unpacked (apt-get download + dpkg-deb -x, no sudo) under ~/opt/pcdeps/root (CMake PCDEPS).
  Vita packages (vdpm, VDPM_NONINTERACTIVE=1): curl (mbedTLS), libarchive (no LZMA: 7z goes through un7zip), un7zip,
  opusfile/opus, mpg123, bzip2, zstd.
- PC converter: test/tools/conv_main.c -> `ghvconv OUT_DIR SOURCE...` (src/songconv.c without the game); its charts were
  compared with pack.py's for the 15 test songs: identical.
  Env: GHV_FIXED=1 (60 Hz virtual clock), GHV_SCRIPT=file (press/release/shot/quit by frame), GHV_SHOTS=dir,
  GHV_SONG=<name or index> GHV_PART GHV_DIFF (straight into a song), GHV_AUTOPLAY=1 (bot). Script lines for the
  downloader: "text <words>" (the keyboard), "waitdl" (holds the script until searches/downloads finish), "sleep <ms>".
- test/run_bot.sh "<song>" part diff [--miss N --overhit N --late MS --sp]: tools/botscript.py presses the real buttons
  for every note; the game logs "result: ..." (2026-10-30: 8 charts full-combo, incl. taps, 694 opens, solos, keys).
- Vita3K can't test it: every vitaGL app stalls there after frame 0, and vitaGL's glGenerateMipmap (sceGxmTransferDownscale)
  hangs it outright (a -DGHV_NO_MIPMAPS build gets further). The real Vita is the test.
- Assets: tools/mkassets.py (atlases game/hud, fonts, backgrounds, highways, sfx from GHWT art + YARG sfx),
  tools/manual.py (sce_sys/manual pages), vita/sce_sys (LiveArea from GHWT logo + title wall).

## notes.vhc (little-endian)
header "VHC1", u32 version=1, ntempo, nbeats, nsections, ntracks, length_ms
tempo  ntempo x {i32 time_us, u32 us_per_quarter}
beats  nbeats x {i32 time_us, u8 kind (0 measure, 1 beat), 3 pad}
sections nsections x {i32 time_us, char name[60]}
tracks ntracks x {u8 part (0 guitar 1 bass 2 rhythm 3 coop 4 keys), u8 diff (0 easy..3 expert), u16 pad, u32 nnotes, nsp, nsolos}
  notes {i32 time_us, u32 len_us, u8 frets (bits G R Y B O, 0x20 open), u8 flags (1 hopo 2 tap 4 sp 8 sp-end), u16 sustain beats x100}
  sp/solos {i32 start_us, i32 end_us} (only ones with notes in this chart)
pack.py CHART_VERSION: bump when the chart output changes; repacking then redoes only charts (audio kept, ~1 s for 15 songs).

## Design (as built)
- Controls (button mode, no strum): layout A Green L, Red Left, Yellow Up/Triangle, Blue Circle, Orange R, open Cross/Down;
  layout B Blue Triangle, open also Circle. Star Power Select or a quick tilt (25 degrees off a slowly-following rest
  direction, any way). Whammy = right stick. Touch: lower screen, nearest cup. Start pauses. Lefty flips lanes and buttons.
- Judging: +-95 ms; a press hits the earliest pending note with that lane; chords when all lanes are in; a press with no note
  near (0.6 s) = overhit; a miss mutes the part's stem until the next hit. Sustains while all their frets are held.
- Scoring: 50 per gem x multiplier (x4, bass x6), 25 per sustain quarter, SP phrase +25%, use at 50%, drains 1/32 per
  quarter, doubles the multiplier; whammy on SP sustains refills 1/32 per quarter. Stars: YARG's current rule (base score =
  perfect run without SP; guitar 0.06 0.12 0.2 0.47 0.78 1.15, bass 0.05 0.1 0.19 0.47 0.78 1.15).
- Look: camera eye (0,4,-5.2) at (0,0,4.8) fov 40 (strikeline y 483, 620 wide; far end y 168); highway board/strings/rails
  fade smoothly over the far 40%. GHWT solo HUD left of the highway: ROCK dome (k 0.72) with 6 SP tubes round its top, score
  LCD on HUD_RM_solo_bottom, streak LCD on HUD_counter_body; multiplier sprite + 5 streak lamps on the gold seat along the
  right rail (2 notes a lamp). Menus: GHWT title/menu/results art, WoR song rows, dialog_bg pause patch.
- Textures: mipmaps made on the CPU (alpha-weighted box filter) and uploaded over vitaGL's chain after glGenerateMipmap +
  sceGxmTransferFinish (vitaGL point-samples the first level of >1024 textures). Memory: newlib heap 96 MB, ~76 MB textures.

## Songs on the Vita (songconv.c, decoder.c, dl.c)
- songconv.c is pack.py's chart logic in C (.chart/.mid incl. Phase Shift opens/taps, HOPOs, SP, solos, the same VHC
  output) plus .sng unpacking; a song keeps its own audio (ogg/opus/mp3/wav stems, up to 16) and gets ghvita.ini
  (song.ini's fields + parts/stems/stem_N/source/chart) and a 256x256 album.jpg. The library reads ghvita.ini, else
  pack.py's song.ini. Raw CH folders and .sng files in songs/ are converted at startup ("ADDING <name>").
- decoder.c: Vorbis, Opus, MP3, WAV to 48 kHz stereo (Catmull-Rom resampling); two decode threads (cores 2 and 1),
  stem s on thread s % 2. 2026-10-30: all 15 test songs full-combo by the bot on the new engine.
- rbcon.c: Xbox 360 STFS packages (.con) holding songs.dta, a .mid and a .mogg per song; each is unpacked into the
  song folder the converter takes, from a download or a file dropped in songs/. Rock Band's encrypted moggs (versions
  11-17, every key index and the five v17 hidden-key sets, HMXA first pages) are decrypted while they stream out by
  moggdec.c, a C port of DarkRTA/themethod3; aes128.c is shared with mega.c. Unencrypted (version 10) audio is copied
  as-is. Songs already unpacked on the card don't need a re-download.
- dl.c (threads at priority 120 on cores 1-2, below the decoders): Chorus Encore (POST api.enchor.us/search, files.enchor.us
  /<md5>.sng) and RhythmVerse (api/ch + api/ps songfiles/search/live, or songfiles/list sorted by update_date when
  nothing is typed; needs 3+ letters; a phrase that finds < 5 also searches its two longest words, 60 each, and keeps
  what has every word). Links: Drive files (drive.usercontent ... &confirm=t), Drive folders (embeddedfolderview lists
  them; only charts/ini/audio/archives/album art are fetched), rhythmverse.co /download_file (escaped), Dropbox (dl=1),
  MediaFire (the page's download button), direct .zip/.rar/.7z/.sng; Mega can't be. 6-fret/drums-only charts are
  flagged before download (Chorus notesData.instruments, RV diff_* fields). Downloads go to ux0:data/ghvita/tmp, unpack
  (libarchive zip/rar/rar5; 7z via un7zip on the Vita), each song found is converted into songs/; the main thread adds
  it to the library when no song list or song is on screen (indexes mustn't move). ghvita.ini's source ("chorus:<md5>",
  "rv:<file id>") marks results IN LIBRARY; same artist+title = HAVE SONG. Google Drive's per-file limit ("Quota
  exceeded" page) can't be got round: the message says try later. TLS: assets/cacert.pem (the PC's CA bundle).
- Vita-only: sceNetInit (1 MB pool) + sceNetCtl on first use (plat_net_up: "no Wi-Fi" when not connected), IME dialog
  for search (vglSwapBuffers(GL_TRUE) while it's up), sceKernelPowerTick(DISABLE_AUTO_SUSPEND) each second while
  downloading. Untested on hardware: if it fails, log.txt has "http:" / "dl:" lines.

## Venues (tools/venue_render.c, tools/venues.py, game.c draw_stage)
- The 40 GHWT venues YargLook decoded (~/mods/yarg-look/YargLook/textures: venue_ghwt_<zone>.wtch + .json marks/facing/
  hide + .parts.json blend per part + .moods.txt) are rendered once on the PC from the audience (10.5 m back from the
  drummer's mark along "facing", 3.4 m up, looking at 0.7 m over the mark, 34 degrees; studios 5.2/2.4/0.8/52) into
  assets/venues/<zone>.jpg (the stage, graded like YargLook's VenueLook) and <zone>_glow.jpg (GHWT's additive light
  parts alone, where the stage doesn't hide them, plus bloom), 960x544, and venues.txt (zone|title|band marks in px).
  `DISPLAY=:77 tools/venues.py ~/mods/yarg-look/YargLook/textures <work dir>` (builds nothing: compile the renderer
  first, `gcc -O2 -o build-tools/venue_render tools/venue_render.c -Isrc -lSDL2 -lGL -lm`).
- Renderer rules (from YargLook's CharacterLoader): opaque parts alpha-tested 0.45, self-lit; soft-alpha "opaque" art
  at 35%, mostly-clear art blended (bright + coloured = light); darkening passes skipped; only the "exposition" mood's
  props; GHWT's crowd_atlas billboards hidden (their atlas is a colour mask). Names are cut at 31 chars in the .wtch:
  textures are found by prefix. The export lost GHWT's material colours, so effect cards (haze, glare, light shafts,
  fire sheets) came out full-bright and whited stages out: each see-through part's share of the picture is measured
  (a quarter-size render of it alone) and capped (5, glare sprites 2, all lights together 14, all haze 14 of 255).
  Per-venue fixes in venues.py: CAMERA_FOR (studios), HIDE_PARTS (Tushino's parked helicopter).
  Debug: VR_PROBE=1 (per-part shares), VR_PICK="x,y ..." (which part is at a pixel), VR_HIDE, VR_HIDE_TEX.
- In the game (cfg.venue: "song" = FNV hash of artist|name picks it, "random", "classic" = the old bg_stage, or a
  zone): the stage picture is drawn tinted a little toward the show's wash colour, its lights added on top in the key
  colour, pulsing on every beat (exp(-5 x beat fraction)); cool palettes changing every two bars, warm ones on every
  beat in a solo or at the top multiplier; Star Power blue-white flashing on the beat; rock meter under a third: dim,
  guttering red. The song card says "at <venue>". Two 960x544 textures, ~4 MB; loaded in game_start.
- rand() is seeded at startup (time ^ clock; GHV_FIXED runs seed 1 ^ the virtual clock, so tests repeat): before,
  QUICKPLAY and "Random" made the same picks every boot.

## Band (tools/band.py, tools/band_render.c, game.c draw_band)
- 18 GHWT members as sprite sheets in assets/band. Guitars and drums: cells the chart picks (band_cell; 8 hand shapes x 6 strum frames, 6 strikes x 8 hit frames, one body pose). Singer: the 100 bpm loop's 4 beats
  that close best, starting on a beat, last beat blended into the first (no jump at the wrap), posed with
  ghwt-tools/wtch_check.py (skin + IK; drummer palms take the hand target's rotation; the singer's right hand holds the mic
  and the mic stays in it, windows with the mic docked avoided), 24 frames, 8x3 cells, filmed 15 degrees down, 256-colour
  PNG. Standing cell 170x256 at 120 px/m, drummer + kit 266x232 at 88 px/m, about their drawn size: band.py and draw_band
  must agree. Each song hashes to one member per role. All members share the beat, each loops only while their own chart part plays (band_playing, vox_singing).
- The singer stands beside the kit, not on their mark (it's behind the highway): draw_band.
- Glow pictures are 480x272 (soft anyway) and skipped when the lights are off.
- The venue pictures use a lens shift of half a screen (venues.py CAMERA) so the marks clear the highway. Frames lock to
  the song's beat, tempo folded to 70-150. No band when Venue is None.

## Vita-only behaviour (the PC build can't show it; check log.txt after the first real run)
- Startup: loading screen (title picture + bar) while pictures load ("loaded: pictures N ms"); the sound effects then
  decode on a thread while the title is up ("loaded: sounds N ms"); a song start waits for them (res_wait_sounds).
- Song list previews open/seek on the decode thread (audio_preview), never the main thread.
- ✕ on GET READY draws a LOADING frame before game_start (chart + highway picture + stems).
- Auto-pause: a frame gap over 1 s (PS button, sleep) or the quick menu coming up (_sceAppMgrGetAppState
  isSystemUiOverlaid, edge only; build version 0x03600011). Tests: script "suspend <ms>" and "overlay on|off".
- Songs: sceKernelPowerTick every frame (no dimming), menu clocks while paused.
- Clocks: menus 333/166/166/111; songs start 444/222/222/166 and a governor (plat_vita.c govern, same rules as the
  FNaF ports) drops to 333/222/166/111 after 10 s calm (no frame over 20 ms, main thread under 6 ms), back to full on
  a late frame or an audio underrun (plat_perf_trouble), retries after 30 s, 60 s ... 5 min. One log line per song
  part: "song frames: N, avg, slowest, late; main thread avg/max; eco clocks %" (nothing is written mid-song).
- GET READY offers only the difficulties the chart has (read_diffs; most custom charts are Expert-only).

## Next
- Drums: PART_DRUMS in songconv only (pads lanes 0-3, kick = open bar, X). tools/pack.py has no drums yet, and songs
  installed before this need downloading again to get the part. Vocals not started (needs mic pitch detection + lyrics).
- Band still plays one 4-beat loop per member: reacting to the song needs more loops per member (sections, solos, rests).
- First thing on hardware: play one song with "Show frame rate" on. If it lags, compare with Band on stage off, then with
  Venue None: that says which layer costs what. Then read ux0:data/ghvita logs if any.
- Real-Vita check: frame rate (clocks 444/222 in songs, 333/166 in menus), audio sync (Calibrate), tilt feel, touch,
  the downloader (Wi-Fi, TLS, IME, a 7z through un7zip), Opus/MP3 stems' decode load (log "song frames").
- The user's Clone Hero library is on the "seilaman" SSD (not connected on 2026-10-30): pack it to the card when it is.

## Drums and input rules (2026-10-30)
- Kick is FRET_OPEN (0x20). Songs converted before this have it in bit 7 and no chart left to redo: split_drums reads both through drum_frets.
- NOTE_TOM_Y/B/G in note flags: toms from .mid notes 110-112 or .chart cymbal notes 66-68 (absent: unknown, blue counts as a tom without pro_drums). kit_pads turns them into the drummer's strikes.
- Rolls: 4+ gems on a pad at most 0.11 s apart, found from the gems. First gem is played; the rest pass if the pad was hit within 0.18 s. Marked on the highway; gaps for freestyle too (+20 x multiplier a hit, 12/s at most).
- Star Power on drums: both cymbals within 50 ms, and not when both are charted.
- PRESS_GRACE 0.13 s: a press soon after its lane's note was hit or missed is never an overhit.
- Vocals use YARG's phrase share (0.325/0.4/0.45/0.575): reaching it is full marks.
- Singer sheet: 32 frames mic at mouth, 16 mic down; vox_singing picks, cross-faded.
- Practice (Square on select), resume countdown, whammy (right stick or rear touch, bends the stem up to MAX_BEND).
- test bots: tools/botscript.py knows drums; GHV_FIXED runs faster than real time, so judge the vocal bot without it.
- Vita link failing with "segment 1 overlaps": segment 0 needs ~5.4 KB free before the next 64 KB boundary. Shrink with -Os or grow past it.

## The show (2026-10-30)

The venue is a picture, and the show is drawn over it in `draw_stage`: screens, haze, the band, the rig's beams, pyro, the crowd, confetti. `assets/stage.png` holds what they need (`tools/mkstage.py`). `member_spot` brings the venues' marks to where they can be seen: some are off the screen or behind the rock meter. The band's sheets get their ink line from `tools/band_ink.py`, run on the sheets as `tools/band.py` made them.

Every other four bars the camera closes in on one of the band, keeping them where they are on the screen, and a solo goes to whoever's playing it.

Clocks: a song loads onto eco clocks and a real late frame sends it to full. Changing them mid-song costs a late frame by itself, which is why the old governor never stayed on eco.

## Fair on a controller (2026-10-30)
- Tremolos (find_tremolos): the same frets over and over under SPAM_GAP 0.12 s, as Rock Band's lanes. The first gem is played, the rest pass while the frets were hit within SPAM_HOLD 0.2 s. Mashing with no timing still fails: the hit window stays +-95 ms and the rule asks for real tapping.
- Slips: PRESS_GRACE and the pull-off rules forgive a finger landing a moment early. Don't widen these further: "dont make the engine too innacurate".
- test/tools/dl_main.c is the real downloader without the game, tools/humanbot.py plays like a person and tools/runs.py compares rule changes.

## Downloads (2026-10-31)
- mega.c: public file and folder links with their own AES (the key is in the link). Folder links were never tested on a live folder: every one found was dead.
- rar.c, ppmd7.c: RAR 3/4 for the solid archives libarchive refuses. Written against the format, checked by each file's CRC; 25 real archives pass and 300 damaged ones neither crash nor hang under ASan.
- Short links (bit.ly, tinyurl) are followed with HEAD, one hop at a time, then treated as whatever they lead to. Song-site links can only reach http and https.

## World Tour (tour.c)
- 8 legs of 5 gigs, every venue once. A gig is 3 songs and an encore, kept in tour.txt once picked. The pool for a leg is its stretch of the library sorted by the part's difficulty; least used first, no band twice on a bill while there's a choice.
- The gig's venue is forced through game_force_venue. tour_on sends select, pause and results back to S_TOUR.
- TOUR_TO_OPEN gigs open the next leg. A played song with no stars still counts as one star, so it's never unplayed again.

## Start-up (2026-10-31)
- Hardware log before this: pictures 6.1 s of about 8 s. tools/rawtex.py and gfx.c load_raw cut the decoding. The vita time hasn't been measured yet: read "loaded: pictures" in log.txt.
- library.bin: Song records keyed on the folder's time. On the Vita the listing has the time, so a known song costs no file access at all. Songs edited in place can keep the folder's time: the list menu's "Read the whole card again" drops the file.

## Downloads and vocals fixes (2026-11-01)
- The download queue never shifts its array: finished and failed slots are reused in place, so the job thread's pointer to the one running stays good, and a full queue says so instead of doing nothing.
- The queue is saved to dl.txt when it changes: waiting jobs carry on at the next start by themselves, failed ones wait on the queue screen. SELECT on the download screen opens it: X retries a failed one, Square removes.
- Downloads keep going during a song; only the unpack-and-shrink waits for it to end.
- Bass reaches x6 and doubles to x12 under Star Power: mkassets composes the x10 and x12 plates GHWT never made (they were the blue-gray squares).
- The vocal arrow runs 70 ms behind the now line, the same window the pitch matching uses, holds its note between phrases instead of snapping to the phrase's middle, and its wake ages while the strip is away.
- The download screen now shows genre and the chart author's note before you download, and the results are sorted by how well the words fit (exact name, then name prefix, then artist, then the rest) instead of the site's order. The library sorts the same way, and numbers sort by value ("Track 2" before "Track 10").

## Settings, career, drums (2026-10-31)
- Options has tabs (L/R: GAME, CONTROLS, LOOK, SOUND; opt_tabs in main.c). New: highway camera (World Tour, Close, High), highway solid, venue brightness, screen flashes, streak messages, timing bar (under the highway, ticks by hit offset), live percent + FC, vanishing notes, wrong note sounds, rear pad whammy, whammy bends the sound, countdown on resume, Reset settings (keeps offset and last song).
- "Check songs are shrunk" (O_SHRINKCHECK) now shrinks too: X counts, and with unshrunk songs found, X again runs them through shrink_song() on a background thread, a song at a time, waiting for any song being played to end. The menus' music is a preview, so it never blocks it.
- Vocals, YARG extras: a vocal note with no lyric at all is the line's clap — drawn as a burst in the lane's centre, hit by any noise above the voice gate (mic_level in vox_pitch); "Singing starts Star Power" (sing_sp, default off, YARG's voice-activated option) lights the meter from a note sung in tune. Pitch windows stay wider than YARG's on purpose: the Vita's mic.
- CAREER (career.c, career.txt): lifetime totals and a title by stars. Counts real plays only: no bot, no practice.
- Results: Square jumps to practice on the toughest named section (not in the tour).
- A sustain let go before its last 0.12 s mutes the part until the next hit, as in YARG. Not a miss.
- Drums: NOTE_KICK2 (flag 0x80) from .mid note 95 / .chart 32, played with Double kick on; only charts converted from now on have it. Drum pads "Five": 4-pad charts with tom/cymbal data (version 2) laid out on five (five_pads in game.c). Results name the weakest pad (10+ gems).
- The streak lamps stand upright on the gold cord, camera-faced like the multiplier plate; the art is the GHWT
  HUD's own glass capsules (tools/lampsrc), patched into the atlas by tools/lampsync.py in CI.
- Drummer sheets rebaked with the cell bottom at -0.62 m: the old -0.35 cut the kit's feet. band.py and draw_band must agree. After a rebake: copy to backups/band_raw, band_ink.py, rawtex.py.
- build-tools/ghvconv can be stale. To test a chart, put the raw folder in songs/ and let the game convert it.

## Held frets, the board, the set (2026-10-31, evening)
- Pressing a fret is the strum here, so a note on a fret already down has nothing to press. pull_off takes any note on a lift, anchored_note plays one as it reaches the line if its frets have been down since before the last note, that note was hit and on other frets, and nothing above is down. A held fret never plays a run of its own note. keepbot in the work dir plays with shared fingers kept down: Island in the Sun 542 to 594 of 595; mash still 0 stars.
- SUS_SLACK 0.09 s, Easy chord sustains (default on).
- Board near black, silver strings, strike bar (hot at top multiplier), cups 1.06 wide, held sustains glow, sparks. draw_rail_meter lies on the board at -HALF_W, in 3D.
- Band: band_strip tips rigid from the feet (cardboard, keep it), floor silhouette, band_wander walks a new spot each 4 bars, solos come out front.
- Failing: FAIL_FRAMES wind-down. Setlists in the Select menu (songs by number, cleared on a rescan or delete). Pause shows progress. Count-in option.

## User's feedback on hardware (2026-10-31)
- The user tests on the Vita and judges there. Sustain drawing and the held cap are the original look on purpose: don't restyle them. Sparks and fire on every struck note are wanted. Lamps stay 3D on the board. Lyrics are on without Sing along.

## Whammy (2026-10-31)
- audio.c: a peak-locked phase vocoder, FFT 2048, overlap 4, a semitone at the bar's bottom as YARG sets it (pitch 2^(-bar/12), only while a sustain rings). Overlap 8 measures the same on real stems at twice the cost.
- Below about 80 Hz a note's harmonics share a lobe and move by the same Hz: the fundamental is right, the ones above up to 2/3 of a semitone out at full bar. Tracking each bin tunes them and costs real bass stems 3-4 dB, so it isn't done.
- whammybar.h is the input. The stick's rest is learnt while no sustain rings, and each rear finger is followed by id: a touch only counts once it's rubbing, so the fingers holding the Vita do nothing.
- Lone-stem rule: a stem with over 85% of the heard power is never muted or bent, checked past the first 3 s.
- The log's "whammy:" lines give the meter earned and each frame's cost on the hardware. A frame must stay well under the 10.7 ms buffer. Whether it does on a Vita has not been measured.
- test/tools/whammy_main.c runs the real mixer on a raw stem (tools/whammy.py reads it), bar_main.c the input. In the PC build, GHV_FIXED + GHV_PACE with SDL's disk audio driver captures the game's own mix, and script line "bar N" holds the whammy.

## Updates from GitHub (update.c, update_apply.c, updater_main.c)
- `tools/release.py --notes "..."` builds HEAD and publishes release v<commit count>: GHVita.vpk, update-full.zip, update-from-<last>.zip with only what changed, and manifest.txt. It refuses a dirty tree or a public repository: the VPK holds art made from your own games.
- The menu asks GitHub once a boot. With a newer release it offers the update: Cross downloads, unpacks and checks every file's size and CRC, and nothing in the app is touched until that passes.
- The GH Vita Updater app (GHVUPD001, `GHVitaUpdater.vpk`, installed once alongside the game) moves the files in with the game closed, the program last, keeping what it replaced in update/old. A failed move puts everything back. Power lost during it carries on at the next start, once, and then the game starts as it is, so it can't get stuck.
- It has to be a separate title: the Vita mounts a running game's own `ux0:app` directory read-only, so the game can't rename its files in place (that was the first updater's Permission denied). Another title gets a writable path, so the game starts the updater with `sceAppMgrLaunchAppByUri` and the updater starts the game again when it's done.
- The repository is private, so the Vita needs a read-only token in ux0:data/ghvita/update.txt: a fine-grained token for this repository with Contents read-only. Without the file nothing is checked. It never goes in the repository or the VPK, and never in the log.
- A manifest may only name eboot.bin, updater.bin and assets/. LiveArea files aren't updated: a new icon still needs the VPK.
- PC test: GHV_UPDATE_API, GHV_UPDATE_TOKEN and GHV_APP stand in for GitHub, the token and the install. The updater app itself has only been run on a Vita.


## Search filters (downloader, 2026-11-03)
- START on the download screen opens a filter panel over the results: charter, artist, genre, instrument,
  difficulty, decade, year and multitrack stems. Chorus gets one row fewer (it has no stems flag). Every
  change re-runs the search; Square clears the row; Start or Circle closes. Rows cycle with Left/Right, text
  rows open the keyboard (kb_apply carries the words back out of kb_update).
- RhythmVerse: POST /api/<sys>/songfiles/search/live with text always 3+ chars, plus author=<shortname> for the
  charter; artist, genre[0], instrument[0], difficulties[0], year, decade=1990 (the decade's first year), audio=full
  all verified live. With no text the /api/<sys>/songfiles/list endpoint takes the same filters and a sort[0]
  (what the newest-charts view already does). rating= is broken server-side and vocals/vocal_parts are rejected.
- The charter and artist need the site's own names: POST /api/all/authors/search/live and /api/all/artists/search/live
  {text,page,records} give shortname/artist_s ids with display names and counts. The panel shows them as a picker
  thread (dl_lookup), Cross picks, Circle drops.
- Chorus Encore: POST api.enchor.us/search/advanced (plain /search otherwise), search="*" when empty, the words
  each as {"value",exact,exclude} objects, instrument as its own spelling (guitarcoop etc), difficulty single
  enum, a decade as minYear..maxYear+9. All verified live against the site (bundled with youtube-dl-era count
  checks: charter 273, +rock 21, +2010s 109).
- Genres come once from GET /api/ch/dictionary/list -> data.filters.genre (slug->label), fetched on a thread the
  first time the panel opens; the same list serves both sites.

## Singing along, the vocal strip, and the anchor (2026-11-03)
- Sing along judged the voice at the INSTRUMENT's difficulty: vox_start(G.diff) took Expert guitar to Expert
  vocal strictness (tolerance 1.0, need 57.5%). Now cfg.sing_diff (Medium default) in Settings -> GAME ->
  "Sing along vocals" is used when singing over an instrument; playing the Vocals part still uses the chosen
  difficulty. Persisted as sing_diff in settings.txt.
- The mic was never the problem: SCE_AUDIO_IN_PORT_TYPE_VOICE 16 kHz mono, 512-sample grains, and the
  "vocals: pitch avg ... ms of 32 ms" log line shows it keeping up.
- vox_draw folded the whole strip away between phrases ("leaves the top of the picture to the band"); the user
  hated it. The strip stays: an instrumental break doesn't take the highway away.
- TFatF played itself: frets held through the song chained the whole chart through anchored_note(). G.act_t marks
  the last time a finger went down or up; a held fret anchors a note only if a finger moved since the note before
  (act_t >= that note's time - WINDOW). Holding all five through Island in the Sun: 0/595, Auto re-press on or off.
  (A fixed 0.15 s grace was tried first: it dropped the held note whenever the other finger lifted early, rg.py
  anchor 12/36 misses; the rule above is 0.)
- Streak lamps matched to footage again (2026-11-05, lampref crops): rope flush on the rail and thinner, short
  domes out of its outer edge, unlit domes dark red, the xN just above the strip.

## Sort by, queue polish, parallel downloads (2026-11-03)
- DlFilter.sort, a Sort row in the filter panel (chorus keeps its no-stems skip by sorting it last before
  Sound). 0 best match; 1-7 sorted here over everything loaded so far (stable by DlHit.order, qsort in
  do_search after each page); 7 charter A-Z and 8 recently added also ask the sites: RV
  sort[0][sort_by]=author/update_date (verified live: 2DHumanity -> Zantor; record_updated descending),
  Chorus {"type":"charter"/"modifiedTime"}. The sites' own systems still come in mixed streams, which is why
  the client re-sorts. dl_filter_text draws the active filters and sort as a dim line under the site tabs.
- RhythmVerse sometimes hands out its errors as 200s: the envelope says "You can't search with this end
  point" and the like, once in a while, on whatever route. rv_fetch asks a second time when it sees them
  (and on 429/5xx run_job does for its downloads too), so the odd pages no longer end the batch.
- A job the queue's dl.txt carried over from an older build can hold its RhythmVerse path raw (the space in
  a file's name ends a curl's host there): run_job sends the path through rvescape once more, idempotent.
- The streak lamp art is the GHWT HUD's own crops (tools/lampsrc, dome up); CI's lampsync step pastes them
  into the carried-on atlas by cell, and mkassets's lamp() reads the same crops for its own regens.
- The queue: three job threads (downloads share the line; the unpack/convert is serialized by install_lock
  since the decoders were built for one). Each job gets its own tmp/job-<id> corner (download.bin and unpacked
  were shared). shrink_defer: the downloader's installs run beside a song being played instead of waiting for
  it to end — what can't convert then stays as it came, the library's shrink catches it later.
- The queue screen shows RV/CH tags, MB left and a frame-sampled MB/s, Triangle = "downloads next"
  (Job.bumps, smallest mark first, dl_prioritize), START clears the finished (dl_clear_done).
- Speed is measured in main.c from the DlJob done/total bytes (dl_jobs_list now fills them), no clock in dl.c.

## Drums on five pads (2026-11-03)
- five_pads() only ran on chart version >= 2 (charts that store tom/cymbal flags), and its no-flag mapping
  stacked the green cymbal onto yellow. Now: a chart that says nothing of toms (plain) keeps its pads on their
  own colours, green up to the fifth lane; a chart with flags keeps pro-drums semantics (default cymbal, tom
  flags move pads to the tom lanes, collisions fall to green's or orange's lane). The version gate is gone;
  true 5-lane charts (fifth-pad notes) were already 5 lanes. 4-lane stays the default; "Drum pads" toggles.

## Drum button layouts (2026-11-04)
- Three of them now (drum_layout 0/1/2): Thumbs as it was, Shoulders reworked so five pads keeps the
  4-pad rule of two buttons per pad (D-pad splits diagonally: yellow Up/Left, low pad Right/Down), and a
  new Guitar layout — each pad on the guitar button of its own colour (Red Left, Yellow Up/Triangle,
  Blue Circle, Green L, Orange R), kick on the open-note buttons X/Down. Colour mapping, not lane
  position, so 4- and 5-pad charts keep the same buttons.
- The options crash: option_text's about[] list is indexed by the option enum; adding O_SINGDIFF in the
  middle shifted every description and left the last one NULL, and the draw doesn't guard. Fixed the
  list and guarded the draw.

## Drum layouts reworked (2026-11-04)
- All three, on the same ideas: pairs wherever a button is free, the kick on a trigger wherever one is
  free, and yellow on Up/Triangle so guitar fingers carry over. Thumbs: snare Left/Down, hats
  Up/Triangle, toms Square/Circle, green L, kick R/X. Shoulders: snare L/Down (it was the only
  single-button pad), hats Up/Left (Right too on four pads), the low pad on Right alone on five pads,
  kick R. Guitar: as colour-for-colour with the guitar, kick R/X/Down on four pads (R is free there),
  X/Down on five.
- The build directory is shared between the sandbox and the machine: each side's cmake wrote its own
  tool paths into the cache and manifest, so every other build re-ran cmake and failed on the other
  side's paths. release.py now builds in build-release, the sandbox keeps build-vita.

## Lamps tipped, RV pages and TLS probes (2026-11-08)
- The domes lean 65 degrees to the right now, the user's pick off the six-orientation sheet (the 40-degree
  first lean read as "still pointing left" to them). The bulbs keep the half-size bulbs v221 shipped; only the
  crops carry the turn (tools/lampsrc mirrors the art and tips it 65 degrees).
- RhythmVerse's API is 1-based and its pages 500 on page=0 for every client: POST
  /api/<sys>/songfiles/list with page=0 answers a 5172-byte HTML "Slim Application Error", page=1+ answers
  healthy JSON, ten in a row. The game already searches at page=1 (dl_search seeds want.page = 1, dl_more
  walks cur_page + 1), so the shipped list/search requests all stand inside the site's own numbering.
  The page-0 crash bites any omitted implementation, not this one.
- TLS probed the way the Vita feels it: rhythmverse.co serves an RSA chain and answers
  ECDHE-RSA-AES128-GCM-SHA256, ECDHE-RSA-AES128-SHA256 and plain RSA-AES128-SHA under TLS 1.2;
  api.enchor.us serves ECDSA only. Enchor working on the Vita proves the stack's ECDSA suites; the RSA ones
  are in vdpm's mbedtls defaults, so the handshake shouldn't be the RV blocker. Everything shorter of that
  (the list body, parse shape, picker keys, download_file route, cacert chain) answers healthy from the
  desk — the device's own status line ("Couldn't search: ...", the curl error string straight from curl)
  is still the one missing fact.

## Camera shots, graphics options, Flash notes, GH3 attacks (2026-11-13)

- Venues: `tools/venues.py --shots <textures> <work>` renders three more shots per venue with venue_render's new yaw/side
  arguments: s1 stage left (yaw +32, 7 m back), s2 stage right (yaw -32), s3 low front (6 m, 0.9 m up); studios closer.
  Each shot's lens shift is solved (three renders) so the drummer lands where the wide shot has them, at the highway's far
  end. Files `<zone>_s<n>.jpg` + `_s<n>_glow.jpg`; venues.txt keeps "zone|title|wide marks" and adds "|marks" per shot
  (old builds read only the first). About 13 MB more on the card, ~7.5 MB of textures while a song plays.
- game.c director (draw_stage): wide for the count-in and after the last note; Star Power starts low (s3); a solo cuts
  to the soloist's near side (guitar s2, bass s1, drums s3); a new named section gets its own angle (choruses/solos/outros
  low); charts without sections cycle every 4 bars. A shot holds 2 bars at least; the camera pushes in 4.5% over 8 bars.
  member_spot keeps every member in the wide shot's safe screen zones; a shot only scales them (0.85..1.3 of wide).
  Shots load in pick_venue (log: "loaded: N venue shots M ms"); freed in game_stop. Options > GRAPHICS > Venue camera:
  "Wide shot only" loads none.
- 30 fps (cfg.frame_rate): play_screen skips game_draw on odd frames and the main loop calls plat_skip_frame (Vita:
  sceDisplayWaitVblankStart, still governed; PC: frame clock advances). game_update still runs every frame, so input and
  judging are unchanged. Draw-time animation steps use G.fstep (ease()/powf) so they keep their speed. The fps readout
  counts drawn frames.
- GRAPHICS tab: preset (Full / Balanced / Battery / Custom shown when nothing matches), frame rate, fps readout, camera,
  note style, effects, band, fire, flashes, crowd, vignette.
- Flash note style (cfg.gem_style 1): flat discs drawn as 18-segment rings (flash_ring), no atlas art: white centre =
  hammer-on, dark = tap, cyan = Star Power; ring targets lit while held with a white ring thrown off on each press; plain
  dark board with lane lines. Drums keep World Tour cups.
- Battle attacks from GH3: BROKEN STRING (the most-used fret of the next 6 s is masked out of held/pressed lanes till
  tapped 5 times), DOUBLE NOTES (single notes in the next 8 s get the neighbouring fret; originals kept in
  battle.doubled and restored by battle_undouble on restart, quit and the next song's start), WHAMMY (highway rolls via
  battle_proj till G.whammy wears battle.whammy_left down, max 10 s), AMP OVERLOAD (6 s shake + blinking gems).
  battle_pick keeps doubles/whammy off drums and string/flip attacks off vocals. PC test hook: GHV_BATTLE=1
  GHV_ATTACK=<n> lands attack n at 4 s (the bot repairs a broken string by pressing the lane).
- Tested on PC only (bot full combos at 30 fps, Flash, still camera; battles with each attack under ASan; regress.sh
  clean). Hardware unknowns: shot load time, texture memory with band + 4 shots, 30 fps frame pacing.

## Ratings, accuracy, a real Flash look, modifiers (2026-11-13, later)

- User: "flash mode is ass", wants more visual settings, a more accurate/cooler/slightly harder engine, more to do.
- Engine: WINDOW Normal 85 ms (was 95), Strict 70, Lenient 110, Pro 55 (cfg.hit_window 3). CHORD_SPREAD 0.08 s: a
  chord key pressed later than that after the first restarts the chord from the frets still held (held frets still
  count). Every hit_note is rated from G.t - note time minus half a frame (buttons read per frame: +8 ms bias seen in the
  bot): Perfect <= 25 ms, Great <= 50, else Good. Accuracy = (P + 0.7 G + 0.4 Good) / notes; rank S+ (98 + every note),
  S 95, A 90, B 80, C 70, D, F failed. Result.accuracy_x10/rank/perfects..; log line "accuracy:"; scores.txt gets the
  accuracy as an 8th field (old builds ignore it) -> Score.acc, rank drawn in the song list and GET READY.
  Compared with v226 on Plug In Baby (tools/humanbot.py seed 3): spread 20 ms same notes; 35 ms -11 notes; 50 ms -13.
  Bot: 100% S+. rg.py 0 misses.
- Flash (from the site's own promo shots: guitarflash.com/site/img/destaque/gf3.jpg): notes and targets drawn in 2D
  after the 3D highway, placed with project() (proj_now, so battle shakes apply): domed caps (dark band, white rim,
  coloured top, shine; hammer-on white middle, tap dark), thick ring targets with black middles lit when held, white
  ring thrown on press. Drums keep World Tour gems. Choosing Note style Flash also sets HUD Flash + Background cover art.
- HUD Flash (cfg.hud_style): rock meter as red/yellow/green bar up the outside of the left rail with a white needle band,
  Star Power bar up the right, multiplier badge under the strikeline, score + streak bottom left; rail lamps hidden.
- Background (cfg.backdrop): venue / song cover (drifting, beat-lit, falls back to dark) / dark with key-colour glow.
- Hit ratings (cfg.hit_ratings 0/1/2 with ms), PERFECT xN for runs of 3+. Highway length (cfg.hw_length: z_far 8.5/11/14.5).
- Modifiers: cfg.sudden_death (miss_note -> rock_change(-2)), cfg.hard_rock (losses doubled in rock_change).
- Achievements: rank_a, rank_s, rank_splus, perfect300, sudden, pro_window, hard_rock (AchPlay has accuracy etc).

## Download audio, Flash look again (2026-11-13, night)

- User: "downloads come with messed up audio", Flash "still bad", improve visuals and engine.
- Audio: every 44.1 kHz download went through a bad resampler: shrink_mogg drew straight lines, the decoder (and so
  shrink_stem) Catmull-Rom. A 10 kHz tone came back with a 13.9 kHz alias at -23 / -28 dB (gritty highs). New
  src/resamp.c: Kaiser windowed sinc, 32 taps, 512 phases (nearest), one table per rate shared by all streams. Alias
  now -66 dB; vs soxr 57.5 dB (Vorbis, was 40.5). Seeks sample-exact (dec_seek pads RS_HALF - 1 frames of silence).
  shrink_mogg mixes, limits (instant attack, 0.25 s release, ceiling 30500) and resamples per group via Resamp.
- MP3: MPG123_FLAGS replaced the defaults and so lost MPG123_GAPLESS: MP3s were 1203 samples (25 ms) late. Fixed.
- Downloader race: job_thread chose a QUEUED job under the lock but run_job only marked it later (after wiping its
  tmp folder): two of the three workers could run one job into one folder. Now claimed before the unlock.
- Old Vita-made stems carry the Opus vendor "ghvita"; new ones "ghvita2". shrink_old(folder) finds them; song list
  menu row 10 "Repair downloaded songs' audio" queues each again (DlUpdateAsk.repair: Chorus by md5 url, RhythmVerse
  found by searching its name with no filters, matched by id); it installs over its own folder (same_source), scores kept.
- Tests: ~/.cache/ghv-work/bin dl_test, dec_test, shrink_test (dectest/*.c); real RB moggs in ~/.cache/ghv-work/dlt/moggs.
- Flash: own camera (G.cam_flash: eye 0,3.35,-4.6, fov 46) when camera 0; lighter board with black lane lines, no
  strings or silver bar, thick white edges; notes are flat pucks with a white top and coloured dot (HOPO all white,
  tap dark); targets coloured rings round dark middles, grey button when held; fire 1.3x, doubled, no smoke frames;
  grey glossy x badge on the strikeline; side bars get a shine stripe.

## Guitar Flash's own rules, GH3 windows (2026-11-13, late)

- Read guitarflash.com's HTML5 engine (mainGame.js, deobfuscated locally, nothing copied) for its rules, put in game.c's GF:
  - clock in 24ths; a note on screen Y s early (1.67/1.96/2.25), falling faster each tick (speed += Z: 0.36/0.26/0.2)
  - taken between y 270 and 350: Easy/Med -125/+83 ms, Hard -125/+125, Expert -167/+125
  - a press arms its fret 2 frames (83 ms); unused = error (streak 0, meter -2/60, overhits++). Chords per lane;
    a single note takes held frets below it. No HOPOs (all notes drawn plain)
  - meter 60 steps from 29: +1 hit, -2 miss/error; multiplier x4 max (bass too); Star Power by the clock 7.4/11.5/15.6 s
- `flash_rules` (GAME) and `flash_motion` (GRAPHICS, 24 fps steps or smooth); time_z() maps its y curve to the highway
- Flash camera lower and wider (eye 2.3/-2.4, fov 64): the highway narrows hard to the top as Flash's does
- hit_window 4 = GH3 (116 early, 100 late, HOPO 83); every window's late end capped at half the gap to the next note
- humanbot Expert spread 20: Flash rules 672/687 and 0 errors with no stray presses; slips and bounces give errors
- 30 fps never worked on the Vita: vglSwapBuffers queues the flip and returns, so only skipped frames waited a vblank and
  drawn frames still came every refresh. Now paced by sceDisplayGetVcount (drawn = last + 2, read-only frame between)
- Flash look redone (v230): notes = black-rimmed white base + coloured dome + shine; targets thick ring, middle lights when pressed; 2D cartoon flames (flash_flames, 3 layers); board near black, thin grey lane lines, no beat lines, no sparks; meters = framed glass tubes filled by level (rock red <19/60, yellow <41, green); solid straight tails

## Flash flat 2D, own art; smooth clock (v231, 2026-11-13)

- Guitar Flash's sprites are its developers' art: not used or traced (only read for layout ideas). Flash note style is
  our own look now, flat: project() maps the board to a screen trapezoid (F2_TOP 118, F2_LINE 466, half-widths 60/252),
  z straight up it, so time_z is Z_FAR * (1 - gfy / y_hit). draw_flash_board: night-blue board with drifting twinkles,
  thin lane lines, bold outlined rails and tails. Notes: black-outlined candy pucks with a twinkle; targets: outlined
  rings with a lens that lights; capsule meters at x = centre +- 344
- audio_song_time smoothed: free-running at song speed, 6% a frame toward the audio anchor, reset over 40 ms or on a
  jump (per-frame clock wobble 0.30 -> 0.04 ms on PC)
- humanbot with no deliberate faults: Plug In Baby guitar 687/687, bass 793/793 (judging is exact)

## GHWT HUD amp, GH Star Power meter rules (v232, 2026-11-14)

- HUD (hud_style 0): dome moved up (dy 204) onto hud_score_body (the amp, 256 art at 0.62): score in its window (lcd
  green), multiplier as hud_score_nixie_{1-4}a / {2,4,6,8}b in the ring (drawn behind the body: the ring is glass),
  font fallback for bass x5/x6 and x10/x12; hud_score_flash on a multiplier rise. Streak counter plate dropped
- SP: activation +0.1 rock; while active hits +0.036 (x2), misses x0.5 (WT rules only; Flash keeps its own)
- Flash notes 0.43 radius, side 0.27, lit rim

## Band grooves, the chart's light show (v233, 2026-11-14)

- tools/band.py: guitar/bass sheets are now GROOVE 16 steps of the 4-beat t100 loop x 3 strum frames (ready, struck,
  through; one hand shape) instead of 8 hand shapes x 6 strums on one frozen pose. game.c band_groove_cell: step from
  the beat phase (tempo folded as before), strum from the chart's last hit; they keep grooving between their parts.
  Card sway for r >= 2 cut to 0.45 (the picture moves now). Sheets regenerated for all 10 (old in
  ~/.cache/ghv-work/band_assets_v232). Re-render: DISPLAY=:77 tools/band.py "<YARG plugins>/textures" <work> <members>
- Lighting cues: songconv keeps [lighting (...)], [bonusfx], [next]/[prev]/[first] from VENUE/EVENTS/LIGHTING tracks
  and .chart Events as \1-marked section names; chart.c splits them into c->cues. Songs converted earlier: cues.c
  reads them from the kept notes.mid/notes.chart, timed by the chart's tempo map (identical to the converter's on
  Chop Suey, 91 cues). draw_stage run_cues: looks verse/chorus/cool/warm (stepped by next), dischord, stomp, harmony,
  frenzy, silhouettes(_spot), searchlights/sweep (lasers), strobe slow/fast (quarters/16ths, Flashes setting
  respected), blackout slow/fast/spot (venue to ~40%, band readable), flare (white flash), bonusfx (pyro), bre.
  SP and the failing red still win. Charts with no cues keep the section-based show
- v234: charts without cues run the same looks from their section names (intro, verse/riff, prechorus/build ->
  verse then strobes on its last bar, chorus -> one stomp bar then chorus + lasers, breakdown stomp, bridge/break
  silhouettes, solo searchlights, outro frenzy, else warm/cool by big). The song's end adds a white flare.

## Flash look removed, Flash is an engine (v235, 2026-11-14)

- The flat 2D Flash look is gone (board, candy notes, rings, flames, capsule meters, Flash HUD, its camera; ~450
  lines). settings: gem_style/hud_style dropped (old gem_style=1 + flash_rules -> engine=1 once).
- GAME: Engine World Tour / Guitar Flash (cfg.engine; GF.on = engine && frets && !battle: gf_judge, its meter, x4,
  timed SP) and Note motion Smooth / 24 fps (cfg.note_steps; time_z quantizes G.t to 1/24 s). Both on the normal
  highway. Bot: clean input FCs 687/687 on both engines and both motions.
- v236: dl.c tidy_thread on downloader start: tmp/covers trimmed to the newest 300 (card had 1922 = 121 MB), job-*
  folders >1 day old with no live job removed. Options help lookup guarded against table gaps. whammy log line
  reports underruns per song (the old number was the session total). Vita dumps Nov 2-4 = already-fixed bugs.
- v237: rail meter redone: rope a thin trim drawn first, lamps on top, each square to the rail's screen direction (+0.22 tip), sized 0.72 of the spacing. Backup ~/.cache/ghv-work/game.c.prerail
- v238: rail lamps laid flat in 3D (flat() on the board plane, outward from the rope, 0.78 of spacing), lit glow flat too; plate at z1+0.12, 0.95 wide. Backup game.c.prerail2

## Rail strip, ratings, x6 nixie (v239, 2026-11-14)
- draw_rail_meter: World Tour's layout from ~/.cache/ghv-work/lampref (cmp.png): hud_rope stretched into a 0.30-wide gold
  strip flush outside the rail (x -HALF_W+0.03 .. -HALF_W-0.27, z 0.45..2.75) on a faint dark bed; five lamp domes end
  to end on it (0.8 of spacing), round ends out; plate at z1+0.22. Backup ~/.cache/ghv-work/game.c.prerail3
- Hit rating text projected at z -0.75 (under the strikeline, clamped to y 500), 25 px, rises 16 px
- tools/nixie_orange.py: hud_score_nixie_6a/8a from World Tour's blue 6b/8b by an RGB map fitted 4b->4a (error 7/255);
  mkassets.py runs it after the hud atlas. x5, x10, x12 still use the drawn fallback
- v240: songs_guess_tiers (songs.c): parts with no diff_ in the ini get a tier from the Expert chart's busyness (80th
  percentile of 8 s windows, notes/s; steps 2.2/3.4/4.6/5.6/6.8/8.2, drums -0.8), Song.guessed bit per part + 0x80 done,
  drawn paler; done lazily in the song list, saved by songs_flush() on every go(). Vocals not estimated.
  Pause: progress plate moved under the patch (y 464); hit rating hidden while paused.

## World Tour's own solo HUD (v241, 2026-11-14)
- User: lamps "flat and shit, worse than ever"; left HUD needed work. Footage crops (lampref Gb7EtSt65xI, 6Eb-bEcOjwk):
  dome on HUD_RM_solo_bottom (score, green LCD) over HUD_counter_body (streak, orange LCD, frame stretched 172x33);
  no multiplier on the left (the rail plate has it). Amp + nixie box gone (backup ~/.cache/ghv-work/game.c.preleft).
  Layout: bar 14,398 186x38; counter 172x33 under it; dome k 0.56 (tubes/glow scaled by ks).
- Rail: screen-space along the projected rail line (z 0.62..2.25, x -HALF_W+0.01): multi_light_seat strip
  (1.9 x step wide) + hud_score_light_{0,1,2}{,_green,_purple,_blue} bulbs (GHWT's own 32x32, flipped, 1.05 x 0.82
  step, 0.36 step out), lit glow 0.55. The capsule lamp_* art and hud_rope are no longer drawn.
- v242 (user: "still badish, especially the lamps"): side-by-side with Gb7EtSt65xI at the same scale. Rail: gold
  seat quad 3.0 x step wide (art's gold is 0.3 of it) at 0.35 step out, bulbs 0.8 x 0.64 step at 0.72 step out (tips
  on the gold's outer edge), glow 0.7. Left: bar 170x36 at 16,400, counter 158x31, dome k 0.66 (0.79 of the bar as
  footage), rim lifted by an additive warm pass, score digits tinted + additive pass (pale lime). Backup game.c.v241
- v243 (user: "3d ish identical to ghwt"): lamps are real 3D geometry in game.c (lamp_seat: twisted gold bar,
  24x10 quads, ridge sin(o + z*22); lamp_bulb: 6x12 dome + glass sleeve pointing -x, r 0.082; lit_quad: per-vertex
  diffuse/ambient/Blinn highlight against cam_eye, convex back-face skip; lit bulbs white-hot core, hue rim, additive
  glow board). z 0.75..2.15, seat x -HALF_W-0.05. ~600 quads a frame: watch Vita frame time. Compare tool:
  ~/.cache/ghv-work/scripts/lampcmp.sh (footage | lit | unlit at the same scale). Backup game.c.v242
- v244 ("better but not great"): close zoom vs 6Eb-bEcOjwk: chain slimmer/longer, bulbs inline with the gold.
  z 0.55..2.15, rb 0.072, seat r 0.045 at -HALF_W-0.045, bulbs 0.045 further out and on top; lit glow 0.95 + two
  glow boards (hue wide, white core)
- v245: Note motion 24 fps was dead since v235 (the Flash-look removal took the stepped paths; only beats and
  sustains still used time_z). draw_now() is the stepped clock: draw_gems, the board scroll (draw_highway), gap/roll
  zones use it. Test: frame diffs on the highway, steps on = big change then ~0 on held frames

## Crowd, singer moves, band lighting (v246, 2026-11-14)
- Crowd: tools/crowd.py bakes GHWT's crowd peds (crowd_ped_01..04, crowd_pedf_1..2 .wtch from YargLook, 25 bones) from
  behind into assets/crowd/<ped>.png: 8 frames x 4 moods (bad, reg_m, good_m, sp_m; the stretch of each clip that
  loops best), 52x100 cells at 46 px/m, rendered by band_render. game.c draw_crowd: three rows (scale 1.6/2.1/2.7,
  feet 565/655/755) in front of the stage, backlit (near-black + the key colour added along the top 55%), mood from
  rock/cheer/SP with a few one mood down, one loop a beat (bored half speed); the old hands strip stays far back
  (draw_crowd_mass). Off with cfg.no_crowd; the rim pass is off with light_fx.
- Singer: tools/band.py gives vocalist sheets 64 frames (8 rows): 0-31 t100 singing, 32-47 GHWT's ns_100 idle between
  lines, 48-63 spjump (32 clip frames round the pelvis peak, every 2nd). Jump frames drop the mic stand (it ran through
  the body) and are lowered by the jump's rise so the cell holds them; game.c singer_rise[] lifts them back (band.py
  prints SINGER_RISE). The singer jumps instead of the card spin when Star Power starts (sheets with 8 rows only).
  Raw sheets in backups/band_raw, inked by band_ink.py; pre-change sheets in ~/.cache/ghv-work/band_assets_v245.
- Band light: band_strip shades top to toe (strip_top/strip_bot: heads take the key colour, legs fall to shade); every
  member has a thin additive rim in the key colour (the camera's one and everyone in SP a strong one).
- Near shots: draw_band shrinks a member (feet fixed) whose head (2.35 m standing, 2.0 the kit) would leave the top of
  the screen: the low SP shot cut heads off.
- Rail lamps: the gold bar ends at the first and last bulbs (it poked out past them).
- v247: every band sheet is 64 frames (8 rows; tools/band.py V_FRAMES). Guitarist/bassist rows 6-7 = spjump like the
  singer (the clip jumps from ~1 m aside: pinned over their groove centre). Jump heights per sheet in
  assets/band/rise.txt (name + 16 metres), read by res.c into res.band_rise; G.band_rise[r]; the lift is capped by
  the room above the head in the shot (a third always shows). Drummer rows 6-7: 48-55 GHWT idle (32-frame loop that
  closes best; while the drum part rests), 56-63 armsup_120 (stick clicks over the kit) looped per 2 beats in the
  count-in and after the last note. Old builds' singer_rise table stays as the fallback.

## Find outdated songs (v248, 2026-11-14)
- Song list SELECT menu, last row (was "Repair downloaded songs' audio", which it includes): every song with a
  chorus:/rv: source is asked about on its site (upd_thread, DlUpdateAsk.check). Outdated = the site lists a part
  (DlHit.parts >= 0) the installed song lacks (Song.parts), or ghvita.ini "chart" < CHART_VERSION (now in songconv.h;
  absent = 1), or shrink_old audio. Outdated ones download again into their own folders (the repair path, scores
  kept); the toast counts missing parts / old charts / old audio, log.txt has "outdated: <song>: <why>" per song.
  Chorus parts come from a name+artist search matched on the md5 (chorus_parts). Songs without a source (raw copies,
  pack.py) aren't checked. LIB_MAGIC bumped (Song.chart_ver). UPD_MAX 200 found per run.
- PC test: GHV_OUTDATED=1 runs the check at start-up; script "sleep 20000 / waitdl" for the downloads. Tested with
  copies of Hash Pipe (vocals taken out of parts: came back) and Go Away (chart = 1: redone); Plug In Baby and
  NeilSSong left alone.
- Not possible: parts no chart has (Smile Like You Mean It's synth: Harmonix's RB chart has no keys anywhere).
- v249: rail lamp chain from z0 + lg/2 - 1.35 rb (flush under the near bulb) to z1 + 0.05 (past the far bulb, as v245:
  the far bulb hides the chain's end, so trimming there left the top lamp "ropeless").
- Tried: GHWT venue moods (VR_MOOD in venue_render): the moods.txt parts are stage props swapped per mood (a riser
  shell, an emblem), not lights; no lights pictures came out. Dropped.
- v250: G.leap_t: the band's jump also fires when a chorus slams in with rock >= 0.5 (the pyro moment), 20 s apart at least (charts split choruses); Star Power sets it too. The old card spin stays for sheets without jump rows.
- v251: band sheets load paletted (gfx.c tex_load_paletted: own 8-bit indexed PNG reader with stbi_zlib_decode_malloc,
  glCompressedTexImage2D GL_PALETTE8_RGBA8_OES on the Vita = native P8 in vitaGL, RGBA on PC; any other PNG or a GL
  error falls back to tex_load). GHV_P8CHECK=1 compares with stb_image on PC (0 bytes differ on every sheet). Band
  memory ~49 MB -> ~12 MB even with the alt sheets. Not seen on hardware yet: if the band is missing or blocky, log.txt
  "tex: paletted ... refused".
- Alt loops: tools/band.py --alt bakes each member's other GHWT loop (band.py ALT: guitar horse/jimi/areyou, bass
  joker/bleed/horse, singer today/areyou/dammit; drummers have none) into assets/band/alt/<name>.png in the main
  sheet's first rows (guitars 48 groove x strum, singer 32 singing). res_band_alt; draw_band "wild" = big section,
  cue chorus/frenzy, SP, or that guitarist's solo: the same cell from the alt sheet. Raw alt sheets in
  backups/band_raw/alt, inked by band_ink.py.
- v252: user wanted the band "look like real GHWT, pick the band, staging and camera, play the actual notes".
  Pick: cfg.band_pick[4] (settings band_pick0..3, "song" = each song's own), O_PICK0..3 in STAGE, thumb picture
  (tools/band_thumbs.py -> assets/band/thumb, res_band_thumb). Close-ups: focus on guitarist/bassist zooms 1.42 (1.55
  in your solo) with the head kept at y 34 (feet may leave the frame), veil over the venue when cam_z > 1.25; singer
  and drummer stand behind the board so they keep the old 1.2 push. Standing sheets 1.5x (band.py STAND 255x384 at
  180 px/m, ink MaxFilter 7); draw_band/band_frame take cells from each sheet's width, so either size works. Play:
  strum held through a sustain, your misses aren't strummed, your whammy shakes your member on a ringing note.
  Not done: fret hand along the neck (needs 5x the frames).
- band3d branch (not on main): the band as GHWT's 3D models. tools/band.py --3d (run with ~/.cache/ghv-venv/bin/python:
  pyfqmr + scipy) writes assets/band3d/<name>.b3d (decimated mesh, 3500 tris, drummer 5500, small parts whole, two bone
  weights) + alt/<name>.b3a + tex/ (<=256 px); the same posed frames as the sheets. src/band3d.c: a worker thread
  (core 1) skins into a back buffer, the main thread draws the front one through a modelview matrix (screen place,
  lean shear, depth slice per member), ink line = second pass along the normals, floor shadow = flattened pass.
  Option "Band look: 3D models / Pictures" (cfg.band_pictures). PC: 1.7 us a member on the main thread.
  Not seen on a Vita yet. assets/band3d is kept in ~/.cache/ghv-work/band3d_assets off the branch (assets/ is
  untracked and the VPK takes everything in it).
- v253 replays + singing (main). src/replay.c: every game_update frame is an RFrame (G.t, held/pressed lanes after
  lefty/touch, whammy bar+dw, SP request bit (select/cymbals/tilt; tilt() now returns the trigger), mic pitch+level
  when singing). game_update -> game_tick(pad, pressed, rf); in a replay game_update runs every frame whose t <= audio
  time (max 8 a frame) with the recorded inputs and t, so it judges and scores the same (PC: mash run 2513 = replay
  2513). Not kept for the bot, practice, battle. Files <player>/replays/<fnv(folder)>_<part><diff>_last.ghr and
  _best.ghr (best unfailed score): head + the Config played with (other sizes tolerated: fields are appended) +
  frames + singing as IMA ADPCM. In a replay cfg is the replay's (your volumes/fps/lights kept), cfg_borrowed stops
  config_save, end_replay restores.
  Singing: vocals.c mic_thread copies grains into a song-time buffer (audio_song_clock, thread-safe, REC_LATENCY 60
  ms); the judge uses vox_feed (the frame's sample) so replays feed the same values; vox_replay(true) = no mic.
  voicefx.c (once at replay load, needs the vocals chart): 90 Hz HP, 6.8 kHz LP, +3 dB at 3 kHz; time shift +-250 ms
  that best lines voiced 10 ms frames up with chart notes; gate (-20 dB) + -10 dB outside notes; tuning = two-head
  delay pitch shifter (W 512) pulling 80% of the way to the note when within 1.5 semitones (octave folded), back to
  a plain W/2 delay otherwise; 3:1 compressor; makeup to ~3600 RMS; 25 ms predelay Freeverb-lite (4 comb + 2
  allpass, stereo spread 11) -> stereo, mixed by audio_voice in mix(). Song's vocals in a replay with singing start
  LOW (0.25), pause menu FULL/LOW/OFF (needs a separate vocals stem). SAVE YOUR SINGING writes a 16 kHz stereo WAV
  to <player>/recordings. Option "Keep my singing in replays" (keep_singing). PC test mic: GHV_MIC=1 (tone) or
  GHV_MIC=<raw s16 16 kHz> + GHV_MIC_LATE=<s>.
- v254: voice styles (voicefx styles[]: Natural/Studio/Big hall/Hard tune = tune strength, range, retune speed,
  reverb wet/fb/damp/predelay; cfg.voice_style, Options > Sound and the replay pause menu; replay.c keeps the raw
  mono to re-mix; game_replay_voice_style detaches the mixer first). Clean view (G.view_mode, replay pause VIEW:
  everything / stage + lyrics / stage only). voicefx: +1e-18 against denormals (4x faster), sin^2 table, compressor
  gain every 16 samples: 130 s mixes in 0.1 s on PC.
- v255: speakers' echo out of the singing (user, Vita speakers + built-in mic: "issues with the vocal recording like
  the mixing the replay and volume"). The mic heard the song; voicefx then gated/aligned/boosted the song along with
  the voice. audio.c mix() keeps what's played (audio_capture: 16 kHz mono, clipped as played, under mix_lock,
  song-time indexed like the mic buffer); vocals.c rec.ref beside rec.pcm; replay_save writes the raw file at once,
  then a PLAT_BACKGROUND thread runs aec_clean and writes last/best again (own copies of info, cfg, frames);
  replay_load / start_replay wait for it ("CLEANING UP YOUR SINGING"). src/aec.c: lag by GCC-PHAT (8192-pt, up to
  64 loud blocks, +-250 ms; peak/mean < 8 = no song in the mic (headphones): left alone); partitioned block
  frequency-domain filter (256-sample blocks, 8 parts = 128 ms, main path 320 samples in), round-robin gradient
  constraint, learning rate from Speex's "leak" (error power vs echo-estimate power regression) so singing doesn't
  teach it the voice; 20 s learning pre-pass; sqrt-Hann spectral post-filter, gain 1 - 2*leak*Py/Pe, floor 0.08.
  Sim (~/.cache/ghv-work/aec: sim.py speaker model, ev.py): song-only parts -33 dB, voice kept x0.99, leftover song
  ~18 dB under the voice while singing; headphones untouched; lags -50..+200 ms, a 20 ms slip recovers in ~10 s.
  0.27 s for 130 s on PC (aec.c -O2 -ffast-math; -O3 broke the Vita link: replay.c went -Os too and segment 0 now
  ends 0x34e9d8, ~220 bytes of room; Vita time unmeasured: log "aec:"). PC test mic: GHV_MIC_ECHO=<gain>
  (+GHV_MIC_ECHO_MS) hears the game's own output (audio_test_heard, song-time history, PC only; the test mic now
  streams grain after grain like hardware); GHV_AEC_DUMP=<prefix> writes mic/ref/clean raw. In-game PC run
  (All They Wanted, NeilSSong vocals as the singer, echo 0.7): lag found steady, song-only parts -10.5 dB, voice kept
  x0.97; the rig's own ceiling for any linear filter is 7-9 dB (its fake speaker clips hard and its clock jitters),
  so the clean figure is the offline sim's. Speaker bleed also confuses the live pitch judge (not fixed: needs this
  in real time).
  Replay "Your singing" is cfg.voice_level (Off/50/75/100/150/200%, saved). voicefx's alignment search started from
  -(1L<<40): long is 32 bits on the Vita, so alignment never moved there; now LONG_MIN.
