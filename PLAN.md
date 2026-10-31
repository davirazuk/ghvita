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
