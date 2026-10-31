# GH Vita. Vita:    cmake -S . -B build-vita -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
#          desktop: cmake -S . -B build-pc   (an SDL2 test build of the same game, see src/plat_sdl.c)
cmake_minimum_required(VERSION 3.10)

if (DEFINED ENV{VITASDK} AND CMAKE_TOOLCHAIN_FILE)
    include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
endif()

project(ghvita C)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-format-truncation -O2")

# the menus' and the downloader's code for size: the executable's code has to end short of a 1 MB boundary, or
# vita-elf-create has no room for its tables ("segment 1 overlaps"), and none of this runs in a song
set_source_files_properties(src/main.c src/shrink.c src/rbcon.c src/moggdec.c src/songconv.c src/dl.c src/net.c src/json.c src/songs.c PROPERTIES COMPILE_OPTIONS -Os)
# its memory is one block read as whatever's in it
set_source_files_properties(src/ppmd7.c PROPERTIES COMPILE_FLAGS -fno-strict-aliasing)
set(SRCS src/main.c src/game.c src/gfx.c src/font.c src/audio.c src/decoder.c src/chart.c src/songs.c src/songconv.c
         src/res.c src/json.c src/net.c src/dl.c src/mega.c src/aes128.c src/moggdec.c src/rar.c src/ppmd7.c src/tour.c src/vocals.c src/rbcon.c src/shrink.c
         src/update.c src/update_apply.c)

# the build's number is how many commits it has: a release is tagged with the same one (tools/release.py)
execute_process(COMMAND git rev-list --count HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE GHV_COMMITS OUTPUT_STRIP_TRAILING_WHITESPACE)
if (NOT GHV_BUILD)
    set(GHV_BUILD ${GHV_COMMITS})
endif()
set_source_files_properties(src/update.c PROPERTIES COMPILE_DEFINITIONS GHV_BUILD=${GHV_BUILD})

if (VITA)
    set(VITA_APP_NAME "GH Vita")
    set(VITA_TITLEID "GHVT00001")
    set(VITA_VERSION "01.00")
    add_definitions(-D__vita__)
    add_executable(ghvita ${SRCS} src/plat_vita.c)
    target_include_directories(ghvita PRIVATE $ENV{VITASDK}/arm-vita-eabi/include/opus)
    target_link_libraries(ghvita
        curl mbedtls mbedx509 mbedcrypto archive un7zip bz2 zstd z
        opusfile opus mpg123 vorbisfile vorbis ogg
        SceNet_stub SceNetCtl_stub SceSysmem_stub SceKernelModulemgr_stub SceProcessmgr_stub
        vitaGL vitashark SceShaccCgExt mathneon taihen_stub SceShaccCg_stub
        SceKernelDmacMgr_stub SceDisplay_stub SceCtrl_stub SceTouch_stub SceMotion_stub
        SceAudio_stub SceSysmodule_stub SceGxm_stub SceCommonDialog_stub SceAppMgr_stub SceAppUtil_stub
        ScePower_stub SceIofilemgr_stub SceKernelThreadMgr_stub SceLibKernel_stub SceHid_stub SceRtc_stub
        SceIme_stub SceAudioIn_stub
        pthread stdc++ m c)
    vita_create_self(eboot.bin ghvita UNSAFE)
    # moves an update in with the game closed
    add_executable(updater src/updater_main.c src/update_apply.c)
    target_link_libraries(updater SceAppMgr_stub SceLibKernel_stub SceKernelThreadMgr_stub SceIofilemgr_stub SceProcessmgr_stub SceSysmem_stub pthread c)
    vita_create_self(updater.bin updater UNSAFE)
    # everything in assets/ goes in the package under assets/
    file(GLOB_RECURSE ASSET_FILES RELATIVE ${CMAKE_SOURCE_DIR} CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/assets/*)
    set(VPK_FILES FILE ${CMAKE_BINARY_DIR}/updater.bin updater.bin)
    foreach(f ${ASSET_FILES})
        list(APPEND VPK_FILES FILE ${CMAKE_SOURCE_DIR}/${f} ${f})
    endforeach()
    file(GLOB_RECURSE SYS_FILES RELATIVE ${CMAKE_SOURCE_DIR}/vita CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/vita/sce_sys/*)
    foreach(f ${SYS_FILES})
        list(APPEND VPK_FILES FILE ${CMAKE_SOURCE_DIR}/vita/${f} ${f})
    endforeach()
    vita_create_vpk(GHVita.vpk ${VITA_TITLEID} eboot.bin
        VERSION ${VITA_VERSION}
        NAME ${VITA_APP_NAME}
        ${VPK_FILES})
else()
    find_package(SDL2 REQUIRED)
    add_executable(ghvita ${SRCS} src/plat_sdl.c)
    # the system has the libraries but not all their headers: the VitaSDK's Vorbis ones (in src/ext/include) are the
    # same API, and the rest come from the -dev packages unpacked under PCDEPS (apt-get download + dpkg-deb -x)
    set(PCDEPS "$ENV{HOME}/opt/pcdeps/root/usr/include" CACHE PATH "unpacked -dev package headers")
    target_include_directories(ghvita PRIVATE ${SDL2_INCLUDE_DIRS} src/ext/include ${PCDEPS} ${PCDEPS}/opus
                               ${PCDEPS}/x86_64-linux-gnu)
    set(L /usr/lib/x86_64-linux-gnu)
    target_link_libraries(ghvita ${SDL2_LIBRARIES} GL z
        ${L}/libcurl.so.4 ${L}/libarchive.so.13
        ${L}/libopusfile.so.0 ${L}/libopus.so.0 ${L}/libmpg123.so.0
        ${L}/libvorbisfile.so.3 ${L}/libvorbis.so.0 ${L}/libogg.so.0 pthread m)
endif()
