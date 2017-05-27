#!/bin/sh -e
pushd work
abi-monitor -get -build -output ../work/serf.json ../src/serf.json
abi-tracker -build ../work/serf.json -deploy ../html
popd
echo `pwd`/html/timeline/serf/index.html
