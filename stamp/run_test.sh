# Inizializzo le variabili relative ai parametri passati in ingresso allo script

#!/bin/bash

maxThread=4
runPerThread=1

echo vacation++ >> results_analysis.txt
nthread=1
while [ $nthread -le $maxThread ] 
	do
	k=0
	while [ $k -lt $runPerThread ]
		do
		echo nice -20 ./vacation/vacation -n4 -q60 -u90 -r1048576 -t4194304 -t$nthread 			
		nice -20 ./vacation/vacation -n4 -q60 -u90 -r1048576 -t4194304 -t$nthread >> results_analysis.txt
		k=$[$k+1]
	done
	nthread=$[$nthread+1]
done

echo genome++ >> results_analysis.txt
nthread=1
while [ $nthread -le $maxThread ] 
	do
	k=0
	while [ $k -lt $runPerThread ]
		do
		echo nice -20 ./genome/genome -g16384 -s64 -n16777216 -t$nthread 			
		nice -20 ./genome/genome -g16384 -s64 -n16777216 -t$nthread >> results_analysis.txt
		k=$[$k+1]
	done
	nthread=$[$nthread+1]
done


echo kmeans >> results_analysis.txt
nthread=1
while [ $nthread -le $maxThread ] 
	do
	k=0
	while [ $k -lt $runPerThread ]
		do
		echo nice -20 ./kmeans/kmeans -m15 -n15 -t0.00001 -i kmeans/inputs/random-n65536-d32-c16.txt -p$nthread
		nice -20 ./kmeans/kmeans -m15 -n15 -t0.00001 -i kmeans/inputs/random-n65536-d32-c16.txt -p$nthread >> results_analysis.txt
		k=$[$k+1]
	done
	nthread=$[$nthread+1]
done




echo intruder >> results_analysis.txt
nthread=1
while [ $nthread -le $maxThread ] 
	do
	k=0
	while [ $k -lt $runPerThread ]
		do
		echo nice -20 ./intruder/intruder -a10 -l128 -n262144 -s1 -t$nthread 			
		nice -20 ./intruder/intruder -a10 -l128 -n262144 -s1 -t$nthread >> results_analysis.txt
		k=$[$k+1]
	done
	nthread=$[$nthread+1]
done





