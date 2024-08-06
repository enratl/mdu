#!/bin/bash
for i in {1..100}
do
	echo $i >> fil
	/usr/bin/time -a -o fil -f "\t%E real" ./mdu /pkg/ -j $i
done
	clear
