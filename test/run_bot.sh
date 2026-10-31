#!/bin/bash
# play one chart with the test player: run_bot.sh "<song folder substring>" part diff [botscript options]
cd "$(dirname "$0")"
song=$1 part=$2 diff=$3; shift 3
dir=$(ls -d songs/*"$song"*/ | head -1)
script=$(mktemp)
python3 ../tools/botscript.py "$dir/notes.vhc" $part $diff "$@" > "$script"
DISPLAY=:77 SDL_AUDIODRIVER=dummy GHV_DATA=data GHV_ASSETS=../assets GHV_SHOTS=shots GHV_FIXED=1 GHV_SONG="$song" \
    GHV_PART=$part GHV_DIFF=$diff GHV_SCRIPT="$script" timeout 600 ../build-pc/ghvita 2>&1 |
    grep -E "^result|^game:|^sustains:|atlas: no|can't"
rm -f "$script"
