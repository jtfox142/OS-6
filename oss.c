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
#define MAX_CHILDREN 100
#define MAX_SIMUL_CHILDREN 18
#define ONE_SECOND 1000000000
#define HALF_SECOND 500000000
#define NO_PF_INCREMENT 100
#define MEM_REQUEST_INCREMENT 14000000
#define PAGE_TABLE_SIZE 32
#define FRAME_TABLE_SIZE 256
#define KB 1024
#define DIRTY 1
#define CLEAN 0

typedef struct msgBuffer {
	long mtype;
	int intData;
	pid_t childPid;
} msgBuffer;

struct Page {
	int frameLocation; //Holds the frame number for this page
	int pendingEntry; //Set to 1 if this page has entered a request but is currently blocked
};

struct Frame {
	int processHeld;
	int pageHeld;
	int dirtyBit;
};

struct PCB {
	int occupied; //either true or false
	pid_t pid; //process id of this child
	int startTimeSeconds; //time when it was created
	int startTimeNano; //time when it was created
	int blocked; //whether this process is able to operate in its present state
	int eventWaitSeconds; //when does its events happen?
	int eventWaitNano; //when does its events happen?
	int memAccessSeconds;
	int memAccessNano;
	int pageFaults;
	struct Page pageTable[PAGE_TABLE_SIZE]; //Each page holds an integer referencing the frame in which it is held
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
//Queue for keeping track of blocked processes
struct Queue *blockedQueue;
//Frame table to hold all the frames
struct Frame frameTable[FRAME_TABLE_SIZE];

// FUNCTION PROTOTYPES

//Help function
void help();

//Process table functions
void initializeProcessTable();
void startInitialProcesses(int initialChildren, int *childCounter);
void initializePCB(pid_t pid);
void outputProcessTable();

//OSS functions
void incrementClock(int timePassed);
void launchChild(int maxSimulChildren, int launchInterval, int *lastLaunchTime, int *childCounter);
void checkForMessages();
void updateTable(pid_t process, msgBuffer rcvbuf);
void nonblockWait();
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
void release(pid_t childPid);
int runDeadlockDetection();

//Page and Frame Functions
void outputFrameTable();
void outputPageTable();

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
void checkTime(int *outputTimer);
void takeAction(pid_t childPid, int msgData);
void childTerminated(pid_t terminatedChild);
void sendMessage(pid_t childPid, int msg);
void deadlockTermination();

//OS-6 Functions
void createFrameTable();
struct Page initializePage();
void frameDefault(int frameNumber);
void outputStatistics(pid_t terminatedChild);
void outputRequest(int chldNum, int chldPid, int address);
void outputPageTable();
void outputFrameTable();
int pageFault(pid_t process, int page);
void processRequest(pid_t childPid, int address);
void removeFromPageTable(pid_t process, int frameNumber);
void addToPageTable(pid_t process, int pageNumber, int frameNumber);
int findPendingPage(pid_t process);
int findEmptyFrame();
void addToFrame(int frameNumber, pid_t process, int pageNumber);
int noEmptyFrame(struct Queue *fifoQueue);
void fifoReplacementAlgo(pid_t incomingProcess, struct Queue *fifoQueue);
void checkEventWait(struct Queue *fifoQueue);

int main(int argc, char** argv) {
	//signals to terminate program properly if user hits ctrl+c or 5 seconds pass
	alarm(5);
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

	int *childCounter = malloc(sizeof(int));
	

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

	if(simul > MAX_SIMUL_CHILDREN) {
		printf("Warning: The maximum value of simul is 18. Terminating.\n");
		terminateProgram(6);
	}
	
	//sets the global var equal to the user arg
	processTableSize = proc;	

	//allocates memory for the processTable stored in global memory
	processTable = calloc(processTableSize, sizeof(struct PCB));
	//sets all pids in the process table to 0
	initializeProcessTable();

	startInitialProcesses(simul, childCounter);

	createFrameTable();

	int *outputTimer = malloc(sizeof(int));
	*outputTimer = 0;

	int *lastLaunchTime = malloc(sizeof(int));
	*lastLaunchTime = 0;

	//Keeps track of blocked PIDs
	blockedQueue = createQueue(processTableSize);
	//Records the order in which frames should be replaced
	struct Queue *fifoQueue = createQueue(FRAME_TABLE_SIZE);

	//stillChildrenToLaunch checks if we have initialized the final PCB yet. 
	//childrenInSystem checks if any PCBs remain occupied
	while(stillChildrenToLaunch() || childrenInSystem()) {
		
		//Nonblocking waitpid to see if a child has terminated.
		nonblockWait();

		//calls another function to check if runningChildren < simul and if timeLimit has been passed
		//if so, it launches a new child.
		launchChild(simul, timelimit, lastLaunchTime, childCounter);

		//Check to see if event wait for a child is now finished
		//If so, its request is granted and its page gets swapped in
		checkEventWait(fifoQueue);

		//checks to see if a message has been sent to parent
		//if so, then it takes proper action
		checkForMessages(fifoQueue); 

		//outputs the process table to a log file and the screen every half second
		//and runs a deadlock detection algorithm every second
		checkTime(outputTimer); //TODO output frame table and shit
	}

	pid_t wpid;
	int status = 0;
	while((wpid = wait(&status)) > 0);
	terminateProgram(SIGTERM);
	free(childCounter);
	free(outputTimer);
	free(lastLaunchTime);
	return EXIT_SUCCESS;
}

/*

TODO
	* Output frame table
	* Cure soft deadlock
	* Terminate if 100 processes have entered system

*/

// FUNCTION DEFINITIONS

//Checks to see if it can wake up any blocked processes
//If so, it puts them into a frame
void checkEventWait(struct Queue *fifoQueue) {
	if(isEmpty(blockedQueue))
		return;

	int blockedQueueSize = blockedQueue->size;
	for(int processCounter = 0; processCounter < blockedQueueSize; processCounter++) {
		int currentPid = dequeue(blockedQueue);
		int entry = findTableIndex(currentPid);

		//Should never happen, but just in case
		if(!processTable[entry].occupied)
			continue;

		if(simulatedClock[0] >= processTable[entry].eventWaitSeconds && simulatedClock[1] > processTable[entry].eventWaitNano) {
			processTable[entry].blocked = 0;
			processTable[entry].eventWaitNano = 0;
			processTable[entry].eventWaitSeconds = 0;
			fifoReplacementAlgo(currentPid, fifoQueue);
			sendMessage(currentPid, 1);
			return;
		}
		enqueue(blockedQueue, currentPid);
	}

}

void fifoReplacementAlgo(pid_t incomingProcess, struct Queue *fifoQueue) {
	int frameToUse;
	//Find the index of the page that is being added to the frame
	int incomingPage = findPendingPage(incomingProcess);
	//If there are no open frames, fetch the (comparatively) first frame to receive an entry
	if(noEmptyFrame(fifoQueue)) {
		frameToUse = dequeue(fifoQueue);
		//Remove the process and page from that frame
		pid_t outgoingProcess = frameTable[frameToUse].processHeld;
		frameDefault(frameToUse); 
		//Remove the frame location from the outgoing process's page table
		removeFromPageTable(outgoingProcess, frameToUse);
		fprintf(fptr, "Clearing frame %d and swapping in P%d page %d\n", frameToUse, findTableIndex(incomingProcess), incomingPage);
	}
	else {
		//Otherwise, use the first empty frame you can find
		frameToUse = findEmptyFrame();
		fprintf("Putting P%d page %d into empty frame %d", findTableIndex(incomingProcess), incomingPage, frameToUse);
	}
	//Should never happen, but just in case
	if(frameToUse < 0 || frameToUse > 255) {
		perror("Error in fifoReplacementAlgo: frame out of bounds.\n");
		exit(1);
	}

	//Add the new page and process to the frame, set the dirty bit to zero
	addToFrame(frameToUse, incomingProcess, incomingPage); 

	int entry = findTableIndex(incomingProcess);
	int address = processTable[entry].pageTable[incomingPage].pendingEntry;
	if(address  < 0) { //If address is negative, then it is a write
		fprintf("Indicating to P%d that a write has happened to address %d\n", entry, address);
		frameTable[frameToUse].dirtyBit = 1;
		fprintf(fptr, "Dirty bit of frame %d set\n");
	}
	else {
		fprintf(fptr, "Giving data from address %d to P%d\n", address, entry);
	}

	//Add the frame location to the page table of the incoming process and remove the pending marker
	addToPageTable(incomingProcess, incomingPage, frameToUse);

	enqueue(fifoQueue, incomingProcess);
}

//If there are no available frames, return 1
int noEmptyFrame(struct Queue *fifoQueue) {
	if(isFull(fifoQueue))
		return 1;
	return 0;
}

//Adds values to a frame
void addToFrame(int frameNumber, pid_t process, int pageNumber) {
	int oldPage = frameTable[frameNumber].pageHeld;
	frameTable[frameNumber].processHeld = process;
	frameTable[frameNumber].pageHeld = pageNumber;
	frameTable[frameNumber].dirtyBit = CLEAN;
}

int findEmptyFrame() {
	for(int frameCount = 0; frameCount < FRAME_TABLE_SIZE; frameCount++) {
		if(frameTable[frameCount].processHeld == -1)
			return frameCount;
	}
	return -1;
}

//Returns the number of the page that is currently awaiting entry into the frame table
int findPendingPage(pid_t process) {
	int entry = findTableIndex(process);
	for(int pageCount = 0; pageCount < PAGE_TABLE_SIZE; pageCount++) {
		if(processTable[entry].pageTable[pageCount].pendingEntry) {
			return(pageCount);
		}
	}

	return -1;
}

void addToPageTable(pid_t process, int pageNumber, int frameNumber) {
	int entry = findTableIndex(process);

	processTable[entry].pageTable[pageNumber].frameLocation = frameNumber;
	processTable[entry].pageTable[pageNumber].pendingEntry = 0;
}

void removeFromPageTable(pid_t process, int frameNumber) {
	int entry = findTableIndex(process);
	
	for(int pageCounter = 0; pageCounter < PAGE_TABLE_SIZE; pageCounter++) {
		if(processTable[entry].pageTable[pageCounter].frameLocation == frameNumber) {
			processTable[entry].pageTable[pageCounter].frameLocation = -1;
			processTable[entry].pageTable[pageCounter].pendingEntry = 0;
			return;
		}
	}
}

void startInitialProcesses(int initialChildren, int *childCounter) {
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
			printf("oss: Launching Child PID %d\n", newChild);
			runningChildren++;
		}
	}

	*childCounter = lowerValue;
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
	release(terminatedChild);

	processTable[entry].occupied = 0;
	processTable[entry].blocked = 0;
	runningChildren--;

	fprintf(fptr, "oss: Child pid %d has terminated and its resources have been released.\n", terminatedChild);
	outputStatistics(terminatedChild);
}

void checkForMessages(struct Queue *fifoQueue) {
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
	else if(rcvbuf.intData == 500) {
		fprintf(fptr, "oss: P%d, PID %d, is planning to terminate.\n", findTableIndex(rcvbuf.childPid), rcvbuf.childPid);
	}
	else if(rcvbuf.childPid != 0) {
		outputRequest(findTableIndex(rcvbuf.childPid), rcvbuf.childPid, rcvbuf.intData);
		processRequest(rcvbuf.childPid, rcvbuf.intData);
	}
}

void processRequest(pid_t childPid, int address) {
	int page = abs(address) / KB;
	int frame = pageFault(childPid, page);
	int entry = findTableIndex(childPid);
	outputRequest(entry, childPid, address);

	if(frame == -1) { //PAGE FAULT: set up eventWait for 14ms, add pendingEntry, enqueue blocked queue
		printf("oss: Oh no, a pagefault! Set up event wait for child here\n");
		fprintf(fptr, "oss: Address %d is not in a frame, pagefault.\n", address);
		processTable[entry].eventWaitSeconds = simulatedClock[0];
		processTable[entry].eventWaitNano = simulatedClock[1] + MEM_REQUEST_INCREMENT;
		if(processTable[entry].eventWaitNano > ONE_SECOND) {
			processTable[entry].eventWaitNano - ONE_SECOND;
			processTable[entry].eventWaitSeconds += 1;
		}
		processTable[entry].pageTable[page].pendingEntry = address;
		processTable[entry].pageFaults += 1;
		enqueue(blockedQueue, childPid);
	}
	else {
		incrementClock(100);
		processTable[entry].memAccessNano += 100;
		if(processTable[entry].memAccessNano >= ONE_SECOND) {
			processTable[entry].memAccessNano -= ONE_SECOND;
			processTable[entry].memAccessSeconds += 1;
		}
		
		if(address <= 0) { //If address is negative, then it is a write
			fprintf(fptr, "oss: Address %d in frame %d, writing data to frame at time %d:%d\n", address, frame, simulatedClock[0], simulatedClock[1]);
			if(frameTable[frame].dirtyBit == 0) {
				frameTable[frame].dirtyBit = 1;
				fprintf(fptr, "Dirty bit of frame %d set\n");
			}
		}
		else {
			fprintf(fptr, "oss: Address %d in frame %d, giving data to P%d at time %d:%d\n", address, frame, entry, simulatedClock[0], simulatedClock[1]);
		}

		sendMessage(childPid, 1);
	}
}

int pageFault(pid_t process, int page) {
	for(int frameCount = 0; frameCount < FRAME_TABLE_SIZE; frameCount++) {
		if(frameTable[frameCount].processHeld == process && frameTable[frameCount].pageHeld == page)
			return frameCount;
	}
	return -1;
}

//Releases held resources in the frameTable and TODO pageTable
void release(pid_t childPid) {
	for(int frameCount = 0; frameCount < FRAME_TABLE_SIZE; frameCount++) {
		if(frameTable[frameCount].processHeld == childPid)
			frameDefault(frameCount);
	}
	fprintf(fptr, "oss: Child pid %d has released its resources.\n", childPid);
}

void sendMessage(pid_t childPid, int msg) {
	buf.intData = msg;
	buf.mtype = childPid;
	fprintf(fptr, "oss: Sending message of %d to child pid %d\n", msg, childPid);
	if(msgsnd(msqid, &buf, sizeof(msgBuffer), 0) == -1) {
		perror("msgsnd to child failed\n");
		terminateProgram(6);
	}
}

void checkTime(int *outputTimer) {
	if(abs(simulatedClock[1] - *outputTimer) >= HALF_SECOND){
		*outputTimer = simulatedClock[1];
		printf("\nOSS PID:%d SysClockS:%d SysClockNano:%d\n", getpid(), simulatedClock[0], simulatedClock[1]); 
		outputProcessTable();
		outputFrameTable();
	}
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

//sets all initial pid values to 0 and ensures processes won't be accidentally blocked
void initializeProcessTable() {
	for(int processCount = 0; processCount < processTableSize; processCount++) {
		processTable[processCount].pid = 0;
		processTable[processCount].eventWaitSeconds = 0;
		processTable[processCount].eventWaitNano = 0;
		processTable[processCount].blocked = 0;
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
	processTable[index].memAccessSeconds = 0;
	processTable[index].memAccessNano = 0;
	processTable[index].pageFaults = 0;
	for(int pageCount = 0; pageCount < PAGE_TABLE_SIZE; pageCount++) {
		processTable[index].pageTable[pageCount] = initializePage();
	}
}

//Checks to see if another child can be launched. If so, it launches a new child.
void launchChild(int maxSimulChildren, int launchInterval, int *lastLaunchTime, int *childCounter) {
	//If the user defined time interval has not been reached, return.
	if((simulatedClock[1] - *lastLaunchTime) < launchInterval)
		return;

	if(checkChildren(maxSimulChildren) && stillChildrenToLaunch()) {
		childCounter += 1;
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
			fprintf(fptr, "oss: Launching new child pid %d.\n", newChild);
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

//TODO
void outputFrameTable() {
	printf("\n***Pretend this is a frame table***\n");
}

//TODO
void outputPageTable() {
	printf("\n***Pretend this is a page table***\n");
}

void outputRequest(int chldNum, int chldPid, int address) {
	//If address is negative then the process is writing
	if(address < 0) {
		fprintf(fptr, "oss: P%d, PID %d, requesting write of address %d at time %d:%d\n", chldNum, chldPid, abs(address), simulatedClock[0], simulatedClock[1]);
	}
	else {
		fprintf(fptr, "oss: P%d, PID %d, requesting read of address %d at time %d:%d\n", chldNum, chldPid, address, simulatedClock[0], simulatedClock[1]);
	}
}

void outputStatistics(pid_t terminatedChild) {
	int entry = findTableIndex(terminatedChild);

	int memAccessTime[2];
	memAccessTime[0] = processTable[entry].memAccessSeconds;
	memAccessTime[1] = processTable[entry].memAccessNano;

	int pageFaults = processTable[entry].pageFaults;

	fprintf("P%d, PID %d, terminated with a memory access time of %d:%d and %d page faults.\n", entry, terminatedChild, memAccessTime[0], memAccessTime[1], pageFaults);
}

void frameDefault(int frameNumber) {
	frameTable[frameNumber].dirtyBit = CLEAN;
	frameTable[frameNumber].processHeld = -1;
	frameTable[frameNumber].processHeld = -1;
}

struct Page initializePage() {
	struct Page page;
	for(int count = 0; count < PAGE_TABLE_SIZE; count++) {
		page.frameLocation = -1;
		page.pendingEntry = 0;
	}

	return page;
}

void createFrameTable() {	
	for(int count = 0; count < FRAME_TABLE_SIZE; count++) {
		frameDefault(count);
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