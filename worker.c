#include<unistd.h>
#include<sys/types.h>
#include<stdio.h>
#include<stdlib.h>
#include<sys/ipc.h>
#include<sys/shm.h>
#include<sys/msg.h>
#include<time.h>

#define PERMS 0644
#define NUMBER_OF_PAGES 32
#define REQUEST_CODE 10
#define TERMINATION_CODE 500
#define MILLISECOND_TIMER 250000000
#define READ 0
#define WRITE 1
#define KB 1024

typedef struct msgbuffer {
	long mtype;
	int intData;
	pid_t childPid;
} msgbuffer;

int RNG(int max) {
	return (rand() % max);
}

int decideAction() {
	int choice = RNG(100);
	if(choice < 60)
		return READ;
	return WRITE;
}

//. If allocations/requests for that resource are maxed out, then it begins
//decrementing until it finds a resource that isn't or has checked all the resources.
int chooseAddress() {
	int address = RNG(NUMBER_OF_PAGES * KB);
	int addressOffset = RNG(1023);
	return address + addressOffset;
}

//Returns 1 if process should terminate
int checkForTermination() {
	int randNum = RNG(100);
	if(randNum < 20) {
		return 1;
	}
	return 0;
}

int main(int argc, char** argv) {
	//get access to shared memory
	const int sh_key = ftok("./oss.c", 0);
	int shm_id = shmget(sh_key, sizeof(int) * 2, IPC_CREAT | 0666);
	int *simulatedClock = shmat(shm_id, 0, 0);

	msgbuffer buf;
	buf.mtype = 1;
	buf.intData = 0;
	int msqid = 0;
	key_t key;

	// get a key for our message queue
	if ((key = ftok("msgq.txt", 1)) == -1) {
		perror("ftok");
		exit(1);
	}

	// create our message queue
	if ((msqid = msgget(key, PERMS)) == -1) {
		perror("msgget in child");
		exit(1);
	}	
       	
	pid_t parentPid = getppid();
	pid_t myPid = getpid();

	unsigned int randval;
	FILE *f;
   	f = fopen("/dev/urandom", "r");
	fread(&randval, sizeof(randval), 1, f);
	fclose(f);

	srand(randval);

	/*
	
	MESSAGE LEGEND

	Sending:
		If msg is positive, then it is a read.
		If msg is negative, then it is a write.
	
	Receiving:
		If the received message is 1, then increment/restart the memory access stopwatch
		If the received message is 2, then pause the memory access time stopwatch and log current time
		
	*/

	int terminate;
	terminate = 0;
	int memAccesses;
	memAccesses = 0;

	msgbuffer rcvbuf;
	while(1) {
		//If worker has run for at least a second,
		//it will check every 250 ms to see if it should 
		//terminate. If it has been alloted all requested resources, 
		//it will terminate naturally. It might also be asked by
		//OSS to terminate if it is deadlocked.
		if((memAccesses % 1000) == 0) {
			terminate = checkForTermination();

			if(terminate) {
				buf.intData = TERMINATION_CODE;
				//printf("WORKER %d: Sending message of %d to master.\n", myPid, buf.intData);
				//printf("WORKER %d: Attempting to terminate.\n", myPid);
				if(msgsnd(msqid, &buf, sizeof(msgbuffer) - sizeof(long), 0) == -1) {
					printf("msgsnd to parent failed.\n");
					exit(1);
				}
				//detach from shared memory
				shmdt(simulatedClock);
				return EXIT_SUCCESS;
			}
		}

		//Send message back to parent
		buf.mtype = parentPid;
		buf.childPid = myPid;

		int action = decideAction();
		int address = chooseAddress();

		if(WRITE == action) {
			buf.intData = -address;
		}

		//printf("WORKER %d: Sending message of %d to master.\n", myPid, buf.intData);
		//Tell parent what we want to do
		if(msgsnd(msqid, &buf, sizeof(msgbuffer), 0) == -1) {
			printf("msgsnd to parent failed.\n");
			exit(1);
		}

		//Get message back from parent
		//printf("WORKER %d: Waiting on reply from master.\n", myPid);
		if(msgrcv(msqid, &rcvbuf, sizeof(msgbuffer), myPid, 0) == -1) {
			printf("msgrcv failure in child %d\n", myPid);
			exit(1);
		}	
		//printf("WORKER %d: Received reply of %d from master.\n", myPid, rcvbuf.intData);
	}


	//printf("WORKER %d: Error: exited loop. Now terminating.\n", myPid);
	//detach from shared memory
	shmdt(simulatedClock);
	return EXIT_SUCCESS;
}
