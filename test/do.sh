#!/bin/zsh
# do.sh <cmd>...: sends each argument as a control command; "wait N" sleeps here, "shot NAME" saves shots/NAME.png plus a 640px copy
T=${0:A:h}
for a in "$@"; do
	set -- ${=a}
	case $1 in
	wait) sleep $2 ;;
	shot) $T/bg.sh cmd shot $T/shots/$2.png; sleep 1.5; sips -Z 640 $T/shots/$2.png --out $T/shots/$2s.png >/dev/null ;;
	*) $T/bg.sh cmd "$a"; sleep 0.3 ;;
	esac
done
