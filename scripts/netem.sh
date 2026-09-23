#!/bin/sh
# add fake loss/delay/reorder on loopback. linux only, needs root.
#   sudo ./scripts/netem.sh on 1%        -> 1% loss
#   sudo ./scripts/netem.sh on 1% 100us  -> 1% loss + 100us delay
#   sudo ./scripts/netem.sh off
set -e
DEV=lo

case "$1" in
on)
    LOSS=${2:-1%}
    DELAY=${3:-0us}
    tc qdisc del dev $DEV root 2>/dev/null || true
    # a little jitter so packets on different paths actually reorder
    tc qdisc add dev $DEV root netem loss "$LOSS" delay "$DELAY" 20us
    tc qdisc show dev $DEV
    ;;
off)
    tc qdisc del dev $DEV root 2>/dev/null || true
    echo "netem off"
    ;;
*)
    echo "usage: $0 on [loss] [delay] | off"
    exit 1
    ;;
esac
