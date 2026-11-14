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
