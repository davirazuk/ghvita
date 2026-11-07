#!/bin/bash
# Smoke test: builds the desktop game, makes the test songs if they're missing, then has autoplay play each part of
# each test song and checks it finishes with every note hit. Exit 0 = good. Used by tools/release.py before uploading.
#   test/smoke.sh            all of it
#   SMOKE_PARTS="0 5" ...    only these parts (0 guitar, 1 bass, 4 keys, 5 drums)
set -u
cd "$(dirname "$0")"
root=..
# the -dev packages' headers are where the system keeps them (CMakeLists' PCDEPS is for a PC without them)
[ -f $root/build-pc/build.ninja ] || cmake -S $root -B $root/build-pc -G Ninja -DPCDEPS=/usr/include >/dev/null
out=$(ninja -C $root/build-pc 2>&1) || { echo "$out" | grep -E "error|FAILED" | head -20; echo "smoke: desktop build failed"; exit 1; }
[ -d songs ] && ls songs | grep -q "GH Vita Tests" || python3 $root/tools/mktestsong.py songs --count 2 >/dev/null
mkdir -p data shots && ln -sfn ../songs data/songs
if [ -z "${DISPLAY:-}" ]; then
    pgrep -f "Xvfb :77" >/dev/null || { Xvfb :77 -screen 0 1280x720x24 >/dev/null 2>&1 & sleep 1; }
    export DISPLAY=:77
fi
fail=0
for song in "Test Song 1" "Test Song 2"; do
    for part in ${SMOKE_PARTS:-0 1 4 5}; do
        log=$(mktemp)
        # the game stays on the results screen: stop it once the result is out
        SDL_AUDIODRIVER=dummy GHV_DATA=data/ GHV_ASSETS=$root/assets/ GHV_SHOTS=shots/ GHV_FIXED=1 GHV_AUTOPLAY=1 \
            GHV_SONG="$song" GHV_PART=$part GHV_DIFF=3 timeout 240 $root/build-pc/ghvita >"$log" 2>&1 &
        pid=$!
        while kill -0 $pid 2>/dev/null && ! grep -q "^result:" "$log"; do sleep 1; done
        sleep 1; kill $pid 2>/dev/null; wait $pid 2>/dev/null; rc=$?
        res=$(grep "^result:" "$log" | head -1)
        if [[ "$res" =~ ^result:\ done.*\ ([0-9]+)/([0-9]+)\ notes ]] && [ "${BASH_REMATCH[1]}" = "${BASH_REMATCH[2]}" ]; then
            echo "ok   $song part $part: ${BASH_REMATCH[1]} notes"
        else
            echo "FAIL $song part $part (exit $rc): ${res:-no result}"; tail -5 "$log" | sed 's/^/     /'; fail=1
        fi
        rm -f "$log"
    done
done
exit $fail
