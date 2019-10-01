# Inizializzo le variabili relative ai parametri passati in ingresso allo script

#!/bin/bash

maxThread=4
runPerThread=1

#------------------------------------------------------------------------------------------------
#------------------------------------------------------------------------------------------------

echo yada >> results_analysis.txt
nthread=1
while [ $nthread -le $maxThread ] 
	do
	nsuppthread=0
	while [ $nsuppthread -le $nthread ] 
		do
		k=0
		while [ $k -lt $runPerThread ]
			do
			#echo nice -20 ./yada/yada -a15 -i yada/inputs/ttimeu1000000.2 -t$nthread -z$nsuppthread 			
			#nice -20 ./yada/yada -a15 -i yada/inputs/ttimeu1000000.2 -t$nthread -z$nsuppthread >> results_analysis.txt
			k=$[$k+1]
		done
		if [ $nsuppthread -eq 0 ]
                then
                        nsuppthread=1
                else
                        nsuppthread=$[$nsuppthread*2]
                fi
	done
	nthread=$[$nthread+1]
done
#------------------------------------------------------------------------------------------------
#------------------------------------------------------------------------------------------------
echo vacation >> results_analysis.txt
nthread=1
while [ $nthread -le $maxThread ] 
	do
	nsuppthread=0
	while [ $nsuppthread -le $nthread ] 
		do
		k=0
		while [ $k -lt $runPerThread ]
			do
			#echo nice -20 ./vacation/vacation -n16 -q60 -u90 -r8192 -t131072 -c$nthread -z$nsuppthread 			
			#nice -20 ./vacation/vacation -n16 -q60 -u90 -r8192 -t131072 -c$nthread -z$nsuppthread >> results_analysis.txt
			k=$[$k+1]
		done
		if [ $nsuppthread -eq 0 ]
                then
                        nsuppthread=1
                else
                        nsuppthread=$[$nsuppthread*2]
                fi
	done
	nthread=$[$nthread+1]
done
#------------------------------------------------------------------------------------------------
#------------------------------------------------------------------------------------------------
echo ssca2 >> results_analysis.txt
nthread=1
while [ $nthread -le $maxThread ] 
	do
	nsuppthread=0
	while [ $nsuppthread -le $nthread ] 
		do
		k=0
		while [ $k -lt $runPerThread ]
			do
			#echo nice -20 ./ssca2/ssca2 -s20 -i1.0 -u1.0 -l3 -p3 -t$nthread -z$nsuppthread			
			#nice -20 ./ssca2/ssca2 -s20 -i1.0 -u1.0 -l3 -p3 -t$nthread -z$nsuppthread  >> results_analysis.txt
			k=$[$k+1]
		done
		if [ $nsuppthread -eq 0 ]
               then
                        nsuppthread=1
                else
                        nsuppthread=$[$nsuppthread*2]
                fi
	done
	nthread=$[$nthread+1]
done
#------------------------------------------------------------------------------------------------
#------------------------------------------------------------------------------------------------
echo labyrinth >> results_analysis.txt
nthread=1
while [ $nthread -le $maxThread ] 
	do
	nsuppthread=0
	while [ $nsuppthread -le $nthread ] 
		do
		k=0
		while [ $k -lt $runPerThread ]
			do
			echo nice -20 ./labyrinth/labyrinth -i labyrinth/inputs/random-x128-y128-z3-n128.txt -t$nthread -s$nsuppthread 			
			nice -20 ./labyrinth/labyrinth -i labyrinth/inputs/random-x128-y128-z3-n128.txt -t$nthread -s$nsuppthread  >> results_analysis.txt
			k=$[$k+1]
		done
		if [ $nsuppthread -eq 0 ]
                then
                        nsuppthread=1
                else
                        nsuppthread=$[$nsuppthread*2]
                fi
	done
	nthread=$[$nthread+1]
done
#------------------------------------------------------------------------------------------------
#------------------------------------------------------------------------------------------------
echo bayes >> results_analysis.txt
nthread=1
while [ $nthread -le $maxThread ] 
	do
	nsuppthread=0
	while [ $nsuppthread -le $nthread ] 
		do
		k=0
		while [ $k -lt $runPerThread ]
			do
			echo nice -20 ./bayes/bayes -v32 -r1024 -n20 -p40 -i2 -e8 -s1 -q0 -t$nthread -z$nsuppthread 			
			nice -20 ./bayes/bayes -v32 -r1024 -n20 -p40 -i2 -e8 -s1 -q0 -t$nthread -z$nsuppthread >> results_analysis.txt
			k=$[$k+1]
		done
		if [ $nsuppthread -eq 0 ]
                then
                        nsuppthread=1
                else
                        nsuppthread=$[$nsuppthread*2]
                fi
	done
	nthread=$[$nthread+1]
done
#------------------------------------------------------------------------------------------------
#------------------------------------------------------------------------------------------------
echo genome >> results_analysis.txt
nthread=1
while [ $nthread -le $maxThread ] 
	do
	nsuppthread=0
	while [ $nsuppthread -le $nthread ] 
		do
		k=0
		while [ $k -lt $runPerThread ]
			do
			echo nice -20 ./genome/genome -g16384 -s64 -n16777216 -t$nthread -z$nsuppthread 			
			nice -20 ./genome/genome -g16384 -s64 -n16777216 -t$nthread -z$nsuppthread >> results_analysis.txt
			k=$[$k+1]
		done
		if [ $nsuppthread -eq 0 ]
                then
                        nsuppthread=1
                else
                        nsuppthread=$[$nsuppthread*2]
                fi
	done
	nthread=$[$nthread+1]
done
#------------------------------------------------------------------------------------------------
#------------------------------------------------------------------------------------------------
echo intruder >> results_analysis.txt
nthread=1
while [ $nthread -le $maxThread ] 
	do
	nsuppthread=0
	while [ $nsuppthread -le $nthread ] 
		do
		k=0
		while [ $k -lt $runPerThread ]
			do
			echo nice -20 ./intruder/intruder -a2 -l16 -n262025 -s1 -t$nthread -z$nsuppthread 			
			nice -20 ./intruder/intruder -a2 -l16 -n262025 -s1 -t$nthread -z$nsuppthread >> results_analysis.txt
			k=$[$k+1]
		done
		if [ $nsuppthread -eq 0 ]
                then
                        nsuppthread=1
                else
                        nsuppthread=$[$nsuppthread*2]
                fi
	done
	nthread=$[$nthread+1]
done
#------------------------------------------------------------------------------------------------
#------------------------------------------------------------------------------------------------
echo kmeans >> results_analysis.txt
nthread=1
while [ $nthread -le $maxThread ] 
	do
	nsuppthread=0
	while [ $nsuppthread -le $nthread ] 
		do
		k=0
		while [ $k -lt $runPerThread ]
			do
			echo nice -20 ./kmeans/kmeans -m12 -n12 -t0.00001 -i kmeans/inputs/random-n65536-d32-c16.txt -p$nthread -z$nsuppthread 			
			nice -20 ./kmeans/kmeans -m12 -n12 -t0.00001 -i kmeans/inputs/random-n65536-d32-c16.txt -p$nthread -z$nsuppthread >> results_analysis.txt
			k=$[$k+1]
		done
		if [ $nsuppthread -eq 0 ]
                then
                        nsuppthread=1
                else
                        nsuppthread=$[$nsuppthread*2]
                fi
	done
	nthread=$[$nthread+1]
done
#------------------------------------------------------------------------------------------------
#------------------------------------------------------------------------------------------------

