# The game as libmain.so, SDL2 from its source beside it. DEPS: a folder with <abi>/include and <abi>/lib holding the
# static libraries the game uses (curl, libarchive, opusfile, opus, vorbis, ogg, mpg123 and what they need), as the
# workflow's vcpkg leaves them.
cmake_minimum_required(VERSION 3.22)
project(ghvita C)
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
add_subdirectory(${SDL_SRC} sdl2)
set(S ${ROOT}/src)
add_library(main SHARED
    ${S}/main.c ${S}/player.c ${S}/achieve.c ${S}/game.c ${S}/gfx.c ${S}/font.c ${S}/audio.c ${S}/decoder.c ${S}/resamp.c
    ${S}/cues.c ${S}/chart.c ${S}/songs.c ${S}/career.c ${S}/songconv.c ${S}/res.c ${S}/json.c ${S}/net.c ${S}/dl.c
    ${S}/mega.c ${S}/aes128.c ${S}/moggdec.c ${S}/rar.c ${S}/ppmd7.c ${S}/tour.c ${S}/vocals.c ${S}/rbcon.c ${S}/shrink.c
    ${S}/band3d.c ${S}/replay.c ${S}/voicefx.c ${S}/aec.c ${S}/update.c ${S}/update_apply.c ${S}/plat_sdl.c)
target_compile_options(main PRIVATE -O2 -Wall -Wno-unused-parameter -Wno-missing-field-initializers)
target_compile_definitions(main PRIVATE GHV_BUILD=${GHV_BUILD})
set_source_files_properties(${S}/ppmd7.c PROPERTIES COMPILE_FLAGS -fno-strict-aliasing)
set_source_files_properties(${S}/band3d.c ${S}/aec.c PROPERTIES COMPILE_OPTIONS "-O3;-ffast-math")
set(D ${DEPS}/${ANDROID_ABI})
target_include_directories(main PRIVATE ${SDL_SRC}/include ${D}/include ${D}/include/opus ${S}/ext/include)
file(GLOB DEP_LIBS ${D}/lib/*.a)
message(STATUS "GH Vita deps in ${D}: ${DEP_LIBS}")
target_link_libraries(main SDL2 -Wl,--start-group ${DEP_LIBS} -Wl,--end-group GLESv1_CM z log android m)
