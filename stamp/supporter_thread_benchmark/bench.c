#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <getopt.h>
#include "map.h"
#include "memory.h"
#include "random.h"
#include "thread.h"
#include "timer.h"
#include "tm.h"

#define NORM		0x7fffffff

unsigned pseed;
double dummy_sum;
int dummy_cycles=0;
int num_trans = 0;
int num_op_per_tran = 0;
float read_prob =1;
int numThread = 1;
int numSupportedThreads = 0;
int arraySize=1;
int *shared_array;

// random number generator between 0 and 1
double random_number(unsigned *pseed){
   	unsigned   temp;
   	double     temp1;
   	temp = (1220703125 * *pseed) & NORM;
   	*pseed = temp;
   	temp1 = temp;
   	temp1 *= 0.465613e-9;
   	return  temp1;
}

//a function for spending some time
void spend_some_time(){
	int cycles=0;
	while(cycles<dummy_cycles) {	
		dummy_sum+=random_number(&pseed);
	 	cycles++;
	}
}

void run (void* argPtr) {

	int txs, ops, i, val=0;
	//initialize tm thread
    	TM_THREAD_ENTER();
    
	for (txs=0; txs<num_trans; txs++) {
		//start transaction
		TM_BEGIN();
	
		for (ops=0; ops<num_op_per_tran; ops++) {
			i=random_number(&pseed)*arraySize;
			if (i==arraySize) i--;
			if (random_number(&pseed)<read_prob) {
				//read a shared variable	
				val=TM_SHARED_READ(shared_array[i]);
			} else {
				//update a shared variable
				TM_SHARED_WRITE(shared_array[i],val);
			}
		}

		//simulating some computation..
		spend_some_time();
		TM_END();
	}	

    	TM_THREAD_EXIT();
}

int main(int argc, char **argv)
{
    	TIMER_T start, stop;
	pseed=1;

	if(argc<=7) {
	        printf("\nYou did not feed me the correct number of arguments.");
		printf("\nUsage: program_name num_trans	num_op_per_tran	read_prob dummy_cycles arraySize numThread numSupportedThreads\nBye bye :( ...\n");
	        exit(1);
	     }  
	num_trans = atoi(argv[1]);
	num_op_per_tran = atoi(argv[2]);
	read_prob = atof(argv[3]);
	dummy_cycles = atoi(argv[4]);
	arraySize = atoi(argv[5]);
	numThread = atoi(argv[6]);
	numSupportedThreads = atoi(argv[7]);
	num_trans=num_trans/numThread;

	shared_array=(int *) malloc(sizeof(int) * arraySize);
	
    	TM_STARTUP(numThread, numSupportedThreads);
  
   	P_MEMORY_STARTUP(numThread);
    	thread_startup(numThread);

    	TIMER_READ(start);
	//run all threads
    	thread_start(run, (void*)NULL);

    	TIMER_READ(stop);

    	printf("\n%i\t%i\t%f", numThread, numSupportedThreads, TIMER_DIFF_SECONDS(start, stop));
   
    	TM_SHUTDOWN();

    	thread_shutdown();



    return 0;
}
