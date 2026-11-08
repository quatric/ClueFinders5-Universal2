#!/bin/zsh
# usage: run.sh [secs] — builds and runs cfrun on the extracted game
T=${0:A:h}
P=${T:h}
(cd "$P/src" && make -j10 DEPS=$P/deps/prefix OUT=$P/build/arm64 2>&1 | grep -E "error:|Error" ) && exit 1
secs=${1:-20}
TS_SUPPORT=$T/support TS_LOG=1 SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy} $P/build/arm64/cfrun "$P/game/ClueFinders 5th Grade/ClueFinders 5th Grade.app" > $T/run.log 2>&1 &
pid=$!; t=0
while kill -0 $pid 2>/dev/null && [ $t -lt $secs ]; do sleep 1; t=$((t+1)); done
kill -9 $pid 2>/dev/null
tail -25 $T/run.log
