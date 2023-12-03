#include<unistd.h>
#include<sys/types.h>
#include<stdio.h>
#include<stdlib.h>
#include<sys/ipc.h>
#include<sys/shm.h>
#include<sys/msg.h>
#include<time.h>

#define PERMS 0644
#define NUMBER_OF_RESOURCES 10
#define REQUEST_CODE 10
#define TERMINATION_CODE 21
#define MILLISECOND_TIMER 250000000
#define REQUEST 0
#define RELEASE 1
#define RESOURCE_INSTANCES 20

typedef struct msgbuffer {
	long mtype;
	int intData;
	pid_t childPid;
} msgbuffer;

struct resourceTracker {
	int requests[NUMBER_OF_RESOURCES];
	int allocations[NUMBER_OF_RESOURCES];
};

int RNG(int max) {
	return (rand() % max);
}

int decideAction() {
	int choice = RNG(100);
	if(choice < 90)
		return REQUEST;
	return RELEASE;
}

//Attempts to grab a random resource. If allocations/requests for that resource are maxed out, then it begins
//decrementing until it finds a resource that isn't or has checked all the resources.
int chooseRequestResource(struct resourceTracker *resourceTracker) {
	int chosenResource = RNG(9);
	int remainingRequests;
	for(int count = 0; count < NUMBER_OF_RESOURCES; count++) {
		remainingRequests = RESOURCE_INSTANCES - resourceTracker->allocations[chosenResource];
		if(resourceTracker->allocations[chosenResource] < 20 && resourceTracker->requests[chosenResource] < remainingRequests)
			return chosenResource;
		if(chosenResource > 0)
			chosenResource--;
		else
			chosenResource = 9;
	}
	return -1;
}

//Attempts to release a random resource. If there are no allocated instances of the chosen resource, then it
//begins decrementing until it finds an instance of a resource or it has checked all the resources.
//If there are no resources to release, then it requests one instead (back in main).
int chooseReleaseResource(struct resourceTracker *resourceTracker) {
	int chosenResource = RNG(9);
	for(int count = 0; count < NUMBER_OF_RESOURCES; count++) {
		if(resourceTracker->allocations[chosenResource] > 0)
			return chosenResource;
		if(chosenResource > 0)
			chosenResource--;
		else
			chosenResource = 9;
	}

	return -1;
}

//Returns 1 if process should terminate
int checkForTermination(struct resourceTracker *resourceTracker) {
	//If there are more requests for a resource than allocations, do not terminate
	for(int count = 0; count < NUMBER_OF_RESOURCES; count++) {
		if(resourceTracker->requests[count] > resourceTracker->allocations[count]) {
			return 0;
		}
	}

	return 1;
}

//returns 0 if another request will surpass the boundaries of resource instances 
int addRequest(struct resourceTracker *resourceTracker, int resourceNumber) {
	if(resourceTracker->requests[resourceNumber] >= 20)
		return 0;
	resourceTracker->requests[resourceNumber] = resourceTracker->requests[resourceNumber] + 1;
	return 1;
}

void addAllocation(struct resourceTracker *resourceTracker, int allocationNumber) {
	int resourceNumber = allocationNumber;
	resourceTracker->allocations[resourceNumber] = resourceTracker->allocations[resourceNumber] + 1;
}

void removeRequest(struct resourceTracker *resourceTracker, int resourceNumber) {
	resourceTracker->requests[resourceNumber] = resourceTracker->requests[resourceNumber] - 1;
}

void removeAllocation(struct resourceTracker *resourceTracker, int allocationNumber) {
	int resourceNumber = allocationNumber - REQUEST_CODE;
	resourceTracker->allocations[resourceNumber] = resourceTracker->allocations[resourceNumber] - 1;
}

void initializeResourceTracker(struct resourceTracker *resourceTracker) {
	for(int count = 0; count < NUMBER_OF_RESOURCES; count++) {
		resourceTracker->allocations[count] = 0;
		resourceTracker->requests[count] = 0;
	}
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

	struct resourceTracker *resourceTracker;
	resourceTracker = malloc(sizeof(struct resourceTracker));
	initializeResourceTracker(resourceTracker);

	unsigned int randval;
	FILE *f;
   	f = fopen("/dev/urandom", "r");
	fread(&randval, sizeof(randval), 1, f);
	fclose(f);

	srand(randval);

	/*
	
	MESSAGE LEGEND

	Sending
		Each message will be comprised of an integer, which can be thought of as two bits.
		The first bit will represent the message type.

		0 = request
		1 = release
		2 = terminate

		The second bit will represent the resource that the process is referring to.

		For example, a message of "16" would indicate that the process is wanting to
		release an instance of resource number 6.

		A message of "9" would indicate that the process is requesting an instance of
		resource number 9.

		Any message that has a value of 20 or more will be taken as a notice of termination.

	Receiving
		If a 2 is received, then the resource has been released successfully and the process continues.
		If a 1 is received, then the requested resource has been granted and the process continues.
		If a 0 is received, then the requested resource has not been granted and the process
		goes to sleep until it can receive that resource, at which point it is sent -1, or it is killed.

	*/

	int timer;
	timer = simulatedClock[1];
	int aliveTime;
	aliveTime = simulatedClock[0];
	int terminate;
	terminate = 0;

	msgbuffer rcvbuf;
	while(1) {
		//If worker has run for at least a second,
		//it will check every 250 ms to see if it should 
		//terminate. If it has been alloted all requested resources, 
		//it will terminate naturally. It might also be asked by
		//OSS to terminate if it is deadlocked.
		aliveTime = simulatedClock[0] - aliveTime;
		if(aliveTime >= 1 && simulatedClock[1] - timer >= MILLISECOND_TIMER) {
			terminate = checkForTermination(resourceTracker);
			timer = simulatedClock[1];

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

		if(REQUEST == action) {
			buf.intData = chooseRequestResource(resourceTracker);
			if(buf.intData == -1) {
				//printf("WORKER %d has requested the maximum number of resources. Terminating\n", myPid);
				exit(0);
			}
		}
		
		if(RELEASE == action) {
			//chooseReleaseResource returns the resource number
			buf.intData = chooseReleaseResource(resourceTracker);

			//If there are no resources to release, request one instead
			if(buf.intData == -1) {
				buf.intData = chooseRequestResource(resourceTracker);
			}
			else //add 10 to communicate that it is being released, not requested
				buf.intData += 10;
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

		//If our request was granted, turn the request into an allocation
		if(1 == rcvbuf.intData) {
			removeRequest(resourceTracker, buf.intData);
			addAllocation(resourceTracker, buf.intData);
		} //If the release was granted, remove the allocation
		else if(2 == rcvbuf.intData) {
			removeAllocation(resourceTracker, buf.intData);
		} //If our request was denied go to "sleep" waiting on a message from parent
		else {
			//printf("WORKER %d: Going to sleep.\n", myPid);
			do {
				if(msgrcv(msqid, &rcvbuf, sizeof(msgbuffer), myPid, 0) == -1) {
					printf("msgrcv failure in child %d\n", myPid);
					exit(1);
				}
			}while(rcvbuf.intData != 1);
			//printf("WORKER %d: Waking up.\n", myPid);
			removeRequest(resourceTracker, buf.intData);
			addAllocation(resourceTracker, buf.intData);
		}
	}


	//printf("WORKER %d: Error: exited loop. Now terminating.\n", myPid);
	//detach from shared memory
	shmdt(simulatedClock);
	return EXIT_SUCCESS;
}
