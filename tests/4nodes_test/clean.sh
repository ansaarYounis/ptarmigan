#!/bin/sh
killall ucoind
bitcoin-cli -conf=`pwd`/regtest.conf -datadir=`pwd`/regtest stop
sleep 1
rm -rf *.cnl node_3333 node_4444 node_5555 node_6666 conf pay_*.conf routing.dot routing.png regtest *.log

# remove synbolic link
rm ucoincli ucoind showdb routing fund-test-in.sh regtest.conf generate.sh
