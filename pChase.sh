#!/bin/bash

pgm=./pChase64_NUMA

b=(8k 16k   24k 32k 48k 64k 96k 128k 192k 256k 384k 512k 768k 1m 1536k  2m  3m  4m  6m  8m 12m 16m )
s=(2m  2m 1536k  1m 96k 64k 48k  32k  24k  16k  12k   8k   6k 1k   768 512 384 256 192 128  96  64 )
c=5

date
uname -a
echo
$pgm -o hdr
for page in 4k 8k 16k
do
    for threads in 1 2
    do
	for refs in 1 2 4
	do
	    for offset in 0 1
	    do
		for access in random "forward 1"
		do
		    for ((i=0; $i < ${#s[*]}; i++))
		    do
			for ((j=0; $j < $c; j++))
			do 
			    $pgm -p $page -t $threads -r $refs -n add $offset -a $access -c ${b[$i]} -i ${s[$i]} -o csv
			done
		    done
	        done
            done
        done
    done
done
echo
cat /proc/cpuinfo
cat /proc/meminfo
date
