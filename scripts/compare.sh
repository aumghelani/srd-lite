#!/bin/sh
# rdt vs tcp ping latency at a few loss rates, using real netem loss.
# linux only, run with sudo. results go to results.txt
set -e
cd "$(dirname "$0")/.."
make -s

N=${N:-20000}
OUT=results.txt
: > $OUT

./build/rdt_echo -p 9000 -P 4 > /dev/null &
RDT=$!
./build/tcp_bench -S -p 9001 > /dev/null &
TCP=$!
trap 'kill $RDT $TCP 2>/dev/null; ./scripts/netem.sh off >/dev/null' EXIT
sleep 0.5

for loss in 0% 0.1% 1% 2% 5%; do
    if [ "$loss" = "0%" ]; then
        ./scripts/netem.sh off > /dev/null
    else
        ./scripts/netem.sh on $loss 50us > /dev/null
    fi
    echo "### loss $loss" | tee -a $OUT
    ./build/rdt_bench -p 9000 -P 4 -n $N | tee -a $OUT
    ./build/tcp_bench -p 9001 -n $N | tee -a $OUT
done
