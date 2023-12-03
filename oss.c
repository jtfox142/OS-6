#include<unistd.h>
#include<sys/types.h>
#include<sys/wait.h>
#include<stdio.h>
#include<stdlib.h>
#include<sys/ipc.h>
#include<sys/shm.h>
#include<time.h>
#include<signal.h>
#include<sys/msg.h>
#include<errno.h>

#define PERMS 0644
#define MAX_CHILDREN 18
#define ONE_SECOND 1000000000
#define HALF_SECOND 500000000
#define STANDARD_CLOCK_INCREMENT 100000
#define RESOURCE_TABLE_SIZE 10

typedef struct msgBuffer {
	long mtype;
	int intData;
	pid_t childPid;
} msgBuffer;

struct PCB {
	int occupied; //either true or false
	pid_t pid; //process id of this child
	int startTimeSeconds; //time when it was created
	int startTimeNano; //time when it was created
	int blocked; //describes which resource this process is waiting for
	int requestVector[10]; //represents how many instances of each resource have been requested
	int allocationVector[10]; //represents how many instances of each resource have been granted
};

struct resource {
	int totalInstances;
	int availableInstances;
	/*
		TODO: Change requestVector to a requestQueue inside of resource.
		It will be a queue of linked lists that hold both the number of the requested resource
		and the simulated time at which it was requested. Then, I can attempt to grant outstanding
		requests first.
	*/
};

//QUEUE CODE ORIGINALLY TAKEN FROM https://www.geeksforgeeks.org/introduction-and-array-implementation-of-queue/
struct Queue {
    int front, rear, size;
    unsigned capacity;
    int *array;
};

// GLOBAL VARIABLES
//For storing each child's PCB. Memory is allocated in main
struct PCB *processTable;
//For storing resources
static struct resource *resourceTable;
//Message queue id
int msqid;
//Needed for killing all child processes
int processTableSize;
//Needed for launching purposes
int runningChildren;
//Output file
FILE *fptr;
//Shared memory variables
int sh_key;
int shm_id;
int *simulatedClock;
//message buffer for passing messages
msgBuffer buf;
//Queue for keeping track of blocked (sleeping) processes
struct Queue *sleepQueue;

// FUNCTION PROTOTYPES

//Help function
void help();

//Process table functions
void initializeProcessTable();
void startInitialProcesses(int initialChildren);
void initializePCB(pid_t pid);
void outputProcessTable();

//OSS functions
void incrementClock(int timePassed);
void launchChild(int maxSimulChildren, int launchInterval, int *lastLaunchTime);
void checkForMessages();
void updateTable(pid_t process, msgBuffer rcvbuf);
void nonblockWait();
void startInitialProcesses(int initialChildren);
void checkOutstandingRequests();

//Program end functions
void terminateProgram(int signum);
void sighandler(int signum);

//Log file functions
void sendingOutput(int chldNum, int chldPid, int systemClock[2]);
void receivingOutput(int chldNum, int chldPid, int systemClock[2], msgBuffer rcvbuf);

//Resource Functions
void grantResource(pid_t childPid, int resourceNumber, int processNumber);
void initializeResourceTable();
void request(pid_t childPid, int resourceNumber);
int release(pid_t childPid, int resourceNumber, int output);
void outputResourceTable();
int runDeadlockDetection();

//Queue functions ORIGINALLY TAKEN FROM https://www.geeksforgeeks.org/introduction-and-array-implementation-of-queue/
struct Queue* createQueue(unsigned capacity);
int isFull(struct Queue* queue);
int isEmpty(struct Queue* queue);
void enqueue(struct Queue* queue, int item);
int dequeue(struct Queue* queue);
int front(struct Queue* queue);
int rear(struct Queue* queue);

//Helper functions
int checkChildren(int maxSimulChildren);
int stillChildrenToLaunch();
int childrenInSystem();
int findTableIndex(pid_t pid);
void checkTime(int *outputTimer, int *deadlockDetectionTimer);
void takeAction(pid_t childPid, int msgData);
void childTerminated(pid_t terminatedChild);
void sendMessage(pid_t childPid, int msg, int output);
void deadlockTermination();

int main(int argc, char** argv) {
	//signals to terminate program properly if user hits ctrl+c or 60 seconds pass
	alarm(60);
	signal(SIGALRM, sighandler);
	signal(SIGINT, sighandler);	

	//allocate shared memory
	sh_key = ftok("./oss.c", 0);
	shm_id = shmget(sh_key, sizeof(int) * 2, IPC_CREAT | 0666);
	if(shm_id <= 0) {
		printf("Shared memory allocation failed\n");
		exit(1);
	}

	//attach to shared memory
	simulatedClock = shmat(shm_id, 0 ,0);
	if(simulatedClock <= 0) {
		printf("Attaching to shared memory failed\n");
		exit(1);
	}

	//set clock to zero
    simulatedClock[0] = 0;
    simulatedClock[1] = 0;

	runningChildren = 0;
	

	//message queue setup
	key_t key;
	system("touch msgq.txt");

	//get a key for our message queue
	if ((key = ftok("msgq.txt", 1)) == -1) {
		perror("ftok");
		exit(1);
	}

	//create our message queue
	if ((msqid = msgget(key, PERMS | IPC_CREAT)) == -1) {
		perror("msgget in parent");
		exit(1);
	}

	//user input variables
	int option;
	int proc;
	int simul;
	int timelimit;

	while ((option = getopt(argc, argv, "hn:s:t:f:")) != -1) {
  		switch(option) {
   			case 'h':
    				help();
    				break;
   			case 'n':
    				proc = atoi(optarg);
    				break;
   			case 's':
				simul = atoi(optarg);
				break;
			case 't':
				timelimit = atoi(optarg);
				break;
			case'f':
				fptr = fopen(optarg, "a");
		}
	}

	if(proc > MAX_CHILDREN) {
		printf("Warning: The maximum value of proc is 18. Terminating.\n");
		terminateProgram(6);
	}
	
	//sets the global var equal to the user arg
	processTableSize = proc;	

	//allocates memory for the processTable stored in global memory
	processTable = calloc(processTableSize, sizeof(struct PCB));
	//sets all pids in the process table to 0
	initializeProcessTable();

	resourceTable = malloc(RESOURCE_TABLE_SIZE * sizeof(struct resource));
	initializeResourceTable();

	startInitialProcesses(simul);

	int *outputTimer = malloc(sizeof(int));
	*outputTimer = 0;

	int *deadlockDetectionTimer = malloc(sizeof(int));
	*deadlockDetectionTimer = 0;

	int *lastLaunchTime = malloc(sizeof(int));
	*lastLaunchTime = 0;

	sleepQueue = createQueue(processTableSize);

	//stillChildrenToLaunch checks if we have initialized the final PCB yet. 
	//childrenInSystem checks if any PCBs remain occupied
	while(stillChildrenToLaunch() || childrenInSystem()) {
		
		//Nonblocking waitpid to see if a child has terminated.
		nonblockWait();

		//calls another function to check if runningChildren < simul and if timeLimit has been passed
		//if so, it launches a new child.
		launchChild(simul, timelimit, lastLaunchTime);

		//Try to grant any outstanding requests 
		checkOutstandingRequests();//TODO

		//checks to see if a message has been sent to parent
		//if so, then it takes proper action
		checkForMessages();

		//outputs the process table to a log file and the screen every half second
		//and runs a deadlock detection algorithm every second
		checkTime(outputTimer, deadlockDetectionTimer);

		incrementClock(STANDARD_CLOCK_INCREMENT);
	}

	pid_t wpid;
	int status = 0;
	while((wpid = wait(&status)) > 0);
	terminateProgram(SIGTERM);
	return EXIT_SUCCESS;
}

// FUNCTION DEFINITIONS

//Checks to see if it can wake up any sleeping processes
//Rechecks if the resource that a worker requested is now available
void checkOutstandingRequests() {
	if(isEmpty(sleepQueue))
		return;

	int sleepQueueSize = sleepQueue->size;
	for(int processCounter = 0; processCounter < sleepQueueSize; processCounter++) {
		int currentPid = dequeue(sleepQueue);
		int entry = findTableIndex(currentPid);

		//Should never happen, but just in case
		if(!processTable[entry].occupied)
			continue;

		for(int resourceCounter = 0; resourceCounter < RESOURCE_TABLE_SIZE; resourceCounter++) {
			if(!processTable[entry].requestVector[resourceCounter])
				continue;
			
			if(resourceTable[resourceCounter].availableInstances > 0) {
				fprintf(fptr, "MASTER: Waking process %d\n", currentPid);
				processTable[entry].blocked = 0;
				grantResource(currentPid, resourceCounter, entry);
				return;
			}
		}
		enqueue(sleepQueue, currentPid);
	}

}

void startInitialProcesses(int initialChildren) {
	int lowerValue;
	(initialChildren < processTableSize) ? (lowerValue = initialChildren) : (lowerValue = processTableSize);
	
	for(int count = 0; count < lowerValue; count++) {
		pid_t newChild;
		newChild = fork();
		if(newChild < 0) {
			perror("Fork failed");
			exit(-1);
		}
		else if(newChild == 0) {
			char fakeArg[sizeof(int)];
			snprintf(fakeArg, sizeof(int), "%d", 1);
			execlp("./worker", fakeArg, NULL);
       		exit(1);
       		}
		else {
			initializePCB(newChild);
			printf("MASTER: Launching Child PID %d\n", newChild);
			runningChildren++;
		}
	}
}

void nonblockWait() {
	int status;
	pid_t terminatedChild = waitpid(0, &status, WNOHANG);

	if(terminatedChild <= 0)
		return;

	childTerminated(terminatedChild);
}

void childTerminated(pid_t terminatedChild) {
	int entry = findTableIndex(terminatedChild);
	for(int count = 0; count < RESOURCE_TABLE_SIZE; count++) {
		while(release(terminatedChild, count, 0));
		processTable[entry].requestVector[count] = 0;
	}
	processTable[entry].occupied = 0;
	processTable[entry].blocked = 0;
	runningChildren--;

	fprintf(fptr, "MASTER: Child pid %d has terminated and its resources have been released.\n", terminatedChild);
}

void checkForMessages() {
	msgBuffer rcvbuf;
	if(msgrcv(msqid, &rcvbuf, sizeof(msgBuffer), getpid(), IPC_NOWAIT) == -1) {
   		if(errno == ENOMSG) {
      		//printf("Got no message so maybe do nothing?\n");
   		}
		else {
				printf("Got an error from msgrcv\n");
				perror("msgrcv");
				terminateProgram(6);
			}
	}
	else if(rcvbuf.childPid != 0) {
		takeAction(rcvbuf.childPid, rcvbuf.intData);
	}
}

void takeAction(pid_t childPid, int msgData) {
	if(msgData < 10) {
		request(childPid, msgData);
		return;
	}
	if(msgData < 20) {
		release(childPid, (msgData - RESOURCE_TABLE_SIZE), 1); //msgData will come back as the resource number + 10
		return;
	}
	childTerminated(childPid);
}

void request(pid_t childPid, int resourceNumber) {
	int entry = findTableIndex(childPid);
	processTable[entry].requestVector[resourceNumber] += 1; //TODO change this to enqueue
	fprintf(fptr, "MASTER: Child pid %d has requested an instance of resource %d\n", childPid, resourceNumber);
	grantResource(childPid, resourceNumber, entry);
}

int release(pid_t childPid, int resourceNumber, int output) {
	int entry = findTableIndex(childPid);
	if(processTable[entry].allocationVector[resourceNumber] > 0) {
		processTable[entry].allocationVector[resourceNumber] -= 1;
		if(output)
			fprintf(fptr, "MASTER: Child pid %d has released an instance of resource %d\n", childPid, resourceNumber);
		resourceTable[resourceNumber].availableInstances += 1;
		sendMessage(childPid, 2, output);
		return 1;
	}
	if(output)
		fprintf(fptr, "MASTER: Child pid %d has attempted to release an instance of resource %d that it does not have\n", childPid, resourceNumber);
	return 0;
}

//Tries to grant the most recent request first and sends a message back to that child.
//After, it attempts to grant any remaining requests in a first process in, first process out order
//NOTE: I know that this is not a very equitable way to do this, and it could lead to starvation.
void grantResource(pid_t childPid, int resourceNumber, int processNumber) {
	//I have a bug where dead processes are granted resources.
	//This is a bad fix, but it is a fix.
	if(!processTable[processNumber].occupied) 
		return;

	buf.mtype = childPid;
	if(resourceTable[resourceNumber].availableInstances > 0) {
		processTable[processNumber].allocationVector[resourceNumber] += 1;
		processTable[processNumber].requestVector[resourceNumber] -= 1;

		resourceTable[resourceNumber].availableInstances -= 1;
		
		fprintf(fptr, "MASTER: Requested instance of resource %d to child pid %d has been granted.\n", resourceNumber, childPid);
		sendMessage(childPid, 1, 1);
	}
	else {
		fprintf(fptr, "MASTER: Requested instance of resource %d to child pid %d has been denied.\n", resourceNumber, childPid);
		sendMessage(childPid, 0, 1);
		int entry = findTableIndex(childPid);
		processTable[entry].blocked = resourceNumber + 1;
		enqueue(sleepQueue, childPid);
	}
}

void sendMessage(pid_t childPid, int msg, int output) {
	buf.intData = msg;
	buf.mtype = childPid;
	if(output)
		fprintf(fptr, "MASTER: Sending message of %d to child pid %d\n", msg, childPid);
	if(msgsnd(msqid, &buf, sizeof(msgBuffer), 0) == -1) {
			perror("msgsnd to child failed\n");
			terminateProgram(6);
	}
}

void checkTime(int *outputTimer, int *deadlockDetectionTimer) {
	if(abs(simulatedClock[1] - *outputTimer) >= HALF_SECOND){
			*outputTimer = simulatedClock[1];
			*deadlockDetectionTimer += 1;
			printf("\nOSS PID:%d SysClockS:%d SysClockNano:%d\n", getpid(), simulatedClock[0], simulatedClock[1]); 
			outputProcessTable();
			outputResourceTable();
	}
	if(2 == *deadlockDetectionTimer) {
		*deadlockDetectionTimer = 0;

		//Terminate processes until deadlock is gone, in order of highest resource allocation. 
		//Terminates processes using the most resources first
		while(runDeadlockDetection()) {
			deadlockTermination();
		}
	}
}

//Kills the most resource-intensive worker process
void deadlockTermination() {
	int heaviestProcess; //records the entry number of the process using the most resources
	int currentResourcesUsed = 0;
	int mostResourcesUsed = 0;
	
	for(int processCounter = 0; processCounter < processTableSize; processCounter++) {
		for(int resourceCounter = 0; resourceCounter < RESOURCE_TABLE_SIZE; resourceCounter++) {
			currentResourcesUsed += processTable[processCounter].allocationVector[resourceCounter];
		}
		if(currentResourcesUsed > mostResourcesUsed) {
			mostResourcesUsed = currentResourcesUsed;
			heaviestProcess = processCounter;
		}
		currentResourcesUsed = 0;
	}
	pid_t workerToTerminate = processTable[heaviestProcess].pid;
	fprintf(fptr, "MASTER: Killing child pid %d to try and correct deadlock.\n", workerToTerminate);
	int sleepQueueSize = sleepQueue->size;
	for(int count = 0; count < sleepQueueSize; count++) {
		int currentPid = dequeue(sleepQueue);
		if(currentPid == workerToTerminate) {
			break;
		}
		enqueue(sleepQueue, currentPid);
	}
	kill(workerToTerminate, 3);
	childTerminated(workerToTerminate);
}

//Returns the entry number of the most resource-intensive process if deadlock is detected, returns 0 otherwise
int runDeadlockDetection() {
	fprintf(fptr, "MASTER: Running deadlock detection algorithm at time %d.%d\n", simulatedClock[0], simulatedClock[1]);
	int requestMatrix[processTableSize][RESOURCE_TABLE_SIZE];
	int allocationMatrix[processTableSize][RESOURCE_TABLE_SIZE];
	int availableVector[RESOURCE_TABLE_SIZE];

	//Construct request and allocation matrices, as well as the available vector
	for(int processCounter = 0; processCounter < processTableSize; processCounter++) {
		for(int resourceCounter = 0; resourceCounter < RESOURCE_TABLE_SIZE; resourceCounter++) {
			requestMatrix[processCounter][resourceCounter] = processTable[processCounter].requestVector[resourceCounter];
			allocationMatrix[processCounter][resourceCounter] = processTable[processCounter].allocationVector[resourceCounter];
			availableVector[resourceCounter] = resourceTable[resourceCounter].availableInstances;
		}
	}



	int satisfyRequest;
	int finished[processTableSize];

	for(int count = 0; count < processTableSize; count++)
		finished[count] = 0;

	//Loop through request matrix, attempting to satisfy requests
	//If available - requested >= 0, then there are sufficient resources for that specific request
	//If satisfyRequest is true, then that process can finish out and then release its resources
	for(int processCounter = 0; processCounter < processTableSize; processCounter++) {
		//If a process has already completed, move on to the next
		if(finished[processCounter])
			continue;

		fprintf(fptr, "Process number: %d\n", processCounter);
		for(int resourceCounter = 0; resourceCounter < RESOURCE_TABLE_SIZE; resourceCounter++) {
			fprintf(fptr, "Resource %d: available: %d requested by process pid %d: %d\n", resourceCounter, availableVector[resourceCounter], processTable[processCounter].pid, requestMatrix[processCounter][resourceCounter]);
			if(availableVector[resourceCounter] - requestMatrix[processCounter][resourceCounter] >= 0) {
				satisfyRequest = 1;
			}
			else {
				satisfyRequest = 0;
				break;
			}
		}
		//If possible, then "let the process play out" and release its resources
		if(satisfyRequest) {
			fprintf(fptr, "MASTER: Simulating the release of resources held by process %d\n", processCounter);//TODO delete
			for(int resourceCounter = 0; resourceCounter < RESOURCE_TABLE_SIZE; resourceCounter++) {
				availableVector[resourceCounter] += allocationMatrix[processCounter][resourceCounter];
			}
			finished[processCounter] = 1;
			processCounter = -1; //Reset the for loop to zero to see if processes at the beginning can now complete
		}
	}

	//There's probably a better way to do this, but this iterates over all indexes of the finished array.
	//If any process remains unfinished at this point, then deadlock has been detected. Otherwise, we
	//are all good
	int deadlockDetected;
	deadlockDetected = 0;
	for(int count = 0; count < processTableSize; count++) {
		if(finished[count] != 1) {
			deadlockDetected = 1;
			fprintf(fptr, "MASTER: Deadlock detected. Taking measures to correct.\n");
			break;
		}
	}

	if(!deadlockDetected) 
		fprintf(fptr, "MASTER: No deadlock detected. Continuing.\n");

	return deadlockDetected;
}

void help() {
    printf("This program is designed to simulate resource management and deadlocks.\n");
	printf("The main process, OSS, launches a user-designated number of child workers, while maintaining a process table,\nresource table, and managing the allocation of resources.\n");
	printf("The child workers can either request or release resources, though they are much more likely to request them.\n");
	printf("Workers have a possibility of terminating after 1 second has passed. They can also be terminated by the parent upon deadlock.\n\n");
    printf("The executable takes four flags: [-n proc], [-s simul], [-t timelimit], and [-f logfile].\n");
    printf("The value of proc determines the total number of child processes to be produced.\n");
	printf("The value of simul determines the number of children that can run simultaneously.\n");
	printf("The value of timelimit determines how often new children may be launched, in nanoseconds.\n");
	printf("The file name provided will be used as a logfile to which this program outputs.\n");
	printf("\nMADE BY JACOB (JT) FOX\nNovember 20th, 2023\n");
	exit(1);
}

//sets all initial pid values to 0
void initializeProcessTable() {
	for(int count = 0; count < processTableSize; count++) {
		processTable[count].pid = 0;
	}
}

//initializes values of the pcb
void initializePCB(pid_t pid) {
	int index;
	index = 0;

	while(processTable[index].pid != 0)
		index++;

	processTable[index].occupied = 1;
	processTable[index].pid = pid;
	processTable[index].startTimeSeconds = simulatedClock[0];
	processTable[index].startTimeNano = simulatedClock[1];
	processTable[index].blocked = 0;
}

void initializeResourceTable() {
	for(int count = 0; count < RESOURCE_TABLE_SIZE; count++) {
		resourceTable[count].availableInstances = 20;
		resourceTable[count].totalInstances = 20;
	}
}

//Checks to see if another child can be launched. If so, it launches a new child.
void launchChild(int maxSimulChildren, int launchInterval, int *lastLaunchTime) {
	//If the user defined time interval has not been reached, return.
	if((simulatedClock[1] - *lastLaunchTime) < launchInterval)
		return;

	if(checkChildren(maxSimulChildren) && stillChildrenToLaunch()) {
		pid_t newChild;
		newChild = fork();
		if(newChild < 0) {
			perror("Fork failed");
			exit(-1);
		}
		else if(newChild == 0) {
			char fakeArg[sizeof(int)];
			snprintf(fakeArg, sizeof(int), "%d", 1);
			execlp("./worker", fakeArg, NULL);
       		exit(1);
       		}
		else {
			initializePCB(newChild);
			*lastLaunchTime = simulatedClock[1];
			fprintf(fptr, "MASTER: Launching new child pid %d.\n", newChild);
			runningChildren++;
		}
	}
}

//Returns true if the number of currently running children is less than the max
int checkChildren(int maxSimulChildren) {
	if(runningChildren < maxSimulChildren)
		return 1;
	return 0;
}

//If the maximum number of children has not been reached, return true. Otherwise return false
int stillChildrenToLaunch() {
	if(processTable[processTableSize - 1].pid == 0) {
		return 1;
	}
	return 0;
}

//Returns 1 if any children are running. Returns 0 otherwise
int childrenInSystem() {
	for(int count = 0; count < processTableSize; count++) {
		if(processTable[count].occupied) {
			return 1;
		}
	}
	return 0;
}

//returns the buffer index corresponding to a given pid
int findTableIndex(pid_t pid) {
	for(int count = 0; count < processTableSize; count++) {
		if(processTable[count].pid == pid)
			return count;
	}
	return 0;
}

void incrementClock(int timePassed) {
	simulatedClock[1] += timePassed;
	if(simulatedClock[1] >= ONE_SECOND) {
		simulatedClock[1] -= ONE_SECOND;
		simulatedClock[0] += 1;
	}
}

void terminateProgram(int signum) {
	//Kills any remaining active child processes
	int count;
	for(count = 0; count < processTableSize; count++) {
		if(processTable[count].occupied)
			kill(processTable[count].pid, signum);
	}

	//Frees allocated memory
	free(processTable);
	processTable = NULL;

	// get rid of message queue
	if (msgctl(msqid, IPC_RMID, NULL) == -1) {
		perror("msgctl to get rid of queue in parent failed");
		exit(1);
	}

	//close the log file
	fclose(fptr);

	//detach from and delete memory
	shmdt(simulatedClock);
	shmctl(shm_id, IPC_RMID, NULL);

	printf("Program is terminating. Goodbye!\n");
	exit(1);
}

void sighandler(int signum) {
	printf("\nCaught signal %d\n", signum);
	terminateProgram(signum);
	printf("If you're seeing this, then bad things have happened.\n");
}

void outputProcessTable() {
	printf("%s\n%-15s %-15s %15s %15s %15s %15s\n", "Process Table:", "Entry", "Occupied", "PID", "StartS", "StartN", "Blocked");
	fprintf(fptr, "%s\n%-15s %-15s %15s %15s %15s %15s\n", "Process Table:", "Entry", "Occupied", "PID", "StartS", "StartN", "Blocked");
	int i;
	for(i = 0; i < processTableSize; i++) {
		printf("%-15d %-15d %15d %15d %15d %15d\n\n", i, processTable[i].occupied, processTable[i].pid, processTable[i].startTimeSeconds, processTable[i].startTimeNano, processTable[i].blocked);
		fprintf(fptr, "%-15d %-15d %15d %15d %15d %15d\n\n", i, processTable[i].occupied, processTable[i].pid, processTable[i].startTimeSeconds, processTable[i].startTimeNano, processTable[i].blocked);
	}
}

void outputResourceTable() {
	printf("%s\n%-15s %-15s %-15s\n", "Resource Table:", "Entry", "Available", "Total");
	fprintf(fptr, "%s\n%-15s %-15s %-15s\n", "Resource Table:", "Entry", "Available", "Total");
	for(int count = 0; count < RESOURCE_TABLE_SIZE; count++) {
		printf("%-15d %-15d %-15d\n", count, resourceTable[count].availableInstances, resourceTable[count].totalInstances);
		fprintf(fptr, "%-15d %-15d %-15d\n", count, resourceTable[count].availableInstances, resourceTable[count].totalInstances);
	}
}

void sendingOutput(int chldNum, int chldPid, int systemClock[2]) {
	fprintf(fptr, "OSS:\t Sending message to worker %d PID %d at time %d:%d\n", chldNum, chldPid, systemClock[0], systemClock[1]);
}

void receivingOutput(int chldNum, int chldPid, int systemClock[2], msgBuffer rcvbuf) {
	if(rcvbuf.intData != 0) {
		fprintf(fptr, "OSS:\t Receiving message from worker %d PID %d at time %d:%d\n", chldNum, chldPid, systemClock[0], systemClock[1]);
	}
	else {
		printf("OSS:\t Worker %d PID %d is planning to terminate.\n", chldNum, chldPid);	
		fprintf(fptr, "OSS:\t Worker %d PID %d is planning to terminate.\n", chldNum, chldPid);	
	}
}


//QUEUE CODE TAKEN FROM https://www.geeksforgeeks.org/introduction-and-array-implementation-of-queue/
// function to create a queue
// of given capacity.
// It initializes size of queue as 0
struct Queue* createQueue(unsigned capacity)
{
    struct Queue* queue = (struct Queue*)malloc(
        sizeof(struct Queue));
    queue->capacity = capacity;
    queue->front = queue->size = 0;
 
    // This is important, see the enqueue
    queue->rear = capacity - 1;
    queue->array = (int*)malloc(
        queue->capacity * sizeof(int));
    return queue;
}
 
// Queue is full when size becomes
// equal to the capacity
int isFull(struct Queue* queue)
{
    return (queue->size == queue->capacity);
}
 
// Queue is empty when size is 0
int isEmpty(struct Queue* queue)
{
    return (queue->size == 0);
}
 
// Function to add an item to the queue.
// It changes rear and size
void enqueue(struct Queue* queue, int item)
{
    if (isFull(queue))
        return;
    queue->rear = (queue->rear + 1)
                  % queue->capacity;
    queue->array[queue->rear] = item;
    queue->size = queue->size + 1;
}
 
// Function to remove an item from queue.
// It changes front and size
int dequeue(struct Queue* queue)
{
    if (isEmpty(queue))
        return 0;
    int item = queue->array[queue->front];
    queue->front = (queue->front + 1)
                   % queue->capacity;
    queue->size = queue->size - 1;
    return item;
}
 
// Function to get front of queue
int front(struct Queue* queue)
{
    if (isEmpty(queue))
        return 0;
    return queue->array[queue->front];
}
 
// Function to get rear of queue
int rear(struct Queue* queue)
{
    if (isEmpty(queue))
        return 0;
    return queue->array[queue->rear];
}