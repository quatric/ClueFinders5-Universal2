#!/bin/zsh
# bg.sh start|cmd <command...>|stop — runs cfrun headless in the background,
# driven through a control FIFO. TS_APP picks another app (the Assessment).
T=${0:A:h}
P=${T:h}
case $1 in
start)
	(cd "$P/src" && make -j10 DEPS=$P/deps/prefix OUT=$P/build/arm64 2>&1 | grep -E "error:") && exit 1
	rm -f $T/ctl; mkfifo $T/ctl
	TS_SUPPORT=$T/support TS_LOG=1 TS_HEADLESS=${TS_HEADLESS-1} TS_CONTROL=$T/ctl SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy} \
		$P/build/arm64/cfrun "${TS_APP:-$P/game/ClueFinders 5th Grade/ClueFinders 5th Grade.app}" > $T/bg.log 2>&1 &
	echo $! > $T/bg.pid ;;
cmd)
	# Writing to the FIFO blocks forever once the runner has exited
	kill -0 $(cat $T/bg.pid 2>/dev/null) 2>/dev/null || { echo "bg.sh: not running" >&2; exit 1; }
	shift; print -r -- "$*" > $T/ctl ;;
stop)
	kill -9 $(cat $T/bg.pid) 2>/dev/null; rm -f $T/bg.pid ;;
esac
