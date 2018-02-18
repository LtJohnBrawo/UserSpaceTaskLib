/*
 * Copyright (c) 2012, Janusz Lisiecki
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the <organization> nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL JANUSZ LISIECKI OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <ucontext.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>

/**********************************/
/* Types */

typedef enum __taskState_t {
	ALLOC = 0,
	READY,
	RUNNING,
	BLOCKED,
	ZOMBIE
} taskState_t;

typedef struct __taskNode_t {
	ucontext_t context;
	taskState_t tState;
	struct __taskNode_t *next;
	struct __taskNode_t *prev;
} taskNode_t;

typedef struct __taskList_t {
	taskNode_t *task;
	struct __taskList_t *next;
	struct __taskList_t *prev;
} taskList_t ;

typedef struct __myMutex_t {
	int value;
	taskNode_t *lockedBy;
	taskList_t taskList;
} myMutex_t ;

/**********************************/
/* Internal functions declarations */
void listInit(taskNode_t *head);
void listAdd(taskNode_t *head, taskNode_t *node);
void listRemove(const taskNode_t *head, taskNode_t *node);
taskNode_t *listGetNext(const taskNode_t *head, const taskNode_t *node);

void schedule(void);
void blockSched(void);
void unblockSched(void);
taskNode_t *switchTasks(void);
taskNode_t *getNextTask(void);

void setCtx(ucontext_t *destCtx, ucontext_t *srcCtx);
static void cleanUpFunc(void);
static void sigHand (int sig, siginfo_t *siginfo, void *vcontext);

/**********************************/
/* User API functions declarations */

void taskLibInit(void);
taskNode_t *createTask(void);
void taskJoin(const taskNode_t *tWait);

void initMyMutex(myMutex_t *mutex);
void lockMutex(myMutex_t *mutex);
myMutex_t *tryLockMutex(myMutex_t *mutex);
void unlockMutex(myMutex_t *mutex);

/**********************************/
/* Global variables */

static taskNode_t tSchedListHead;
static taskNode_t *currTask;
static taskNode_t *mainTask;
static ucontext_t cleanUpCtx;

/**********************************/
/* Functions definitions */

void initMyMutex(myMutex_t *mutex) {
	mutex->value = 0;
	mutex->taskList.next = &(mutex->taskList);
	mutex->taskList.prev = &(mutex->taskList);
	mutex->taskList.task = NULL;
}

void lockMutex(myMutex_t *mutex) {
	blockSched();
	if (mutex->value != 0) {
		//add to waiting queue
		taskList_t myNode;
		myNode.task = currTask;
		mutex->taskList.prev->next = &myNode;
		mutex->taskList.prev = &myNode;
		myNode.prev = mutex->taskList.prev;
		myNode.next = &(mutex->taskList);

		while (mutex->value != 0) {
			currTask->tState = BLOCKED;
			unblockSched();
			schedule();
			blockSched();
		}

		//remove from waiting queue
		myNode.prev->next = myNode.next;
		myNode.next->prev = myNode.prev;
	}
	mutex->value = 1;
	mutex->lockedBy = currTask;
	unblockSched();
}

myMutex_t *tryLockMutex(myMutex_t *mutex) {
	myMutex_t * ret = NULL;
	blockSched();
	if (mutex->value == 0) {
		mutex->value = 1;
		ret = mutex;
	}
	unblockSched();
	return ret;
}

void unlockMutex(myMutex_t *mutex) {
	blockSched();
	if (mutex->value != 0 && mutex->lockedBy == currTask) {
		mutex->value = 0;
		//notify all waiting processes
		taskList_t *nextT = mutex->taskList.next;
		while (nextT != &(mutex->taskList)) {
			nextT->task->tState = READY;
			nextT = nextT->next;
		}
	}
	unblockSched();
}

void listInit(taskNode_t *head) {
	head->next = head;
	head->prev = head;
}

void listAdd(taskNode_t *head, taskNode_t *node) {
	head->prev->next = node;
	node->prev = head->prev;
	node->next = head;
	head->prev = node;
}

void listRemove(const taskNode_t *head, taskNode_t *node) {
	if (head != node) {
		node->next->prev = node->prev;
		node->prev->next = node->next;
	}
}

taskNode_t *listGetNext(const taskNode_t *head, const taskNode_t *node) {
	taskNode_t *nextNode = node->next;
	while (nextNode == head) {
		nextNode = nextNode->next;
	}
	return nextNode;
}

void taskLibInit(void) {
	struct sigaction sigH = {0,};
	sigH.sa_sigaction = &sigHand;
	sigH.sa_flags = SA_SIGINFO;
	sigaction(SIGALRM, &sigH, NULL);

	listInit(&tSchedListHead);

	//create current, main task
	currTask = (taskNode_t*) calloc(1, sizeof(taskNode_t));
	mainTask = currTask;
	getcontext(&(currTask->context));
	listAdd(&tSchedListHead, currTask);

	//create clean up context
	getcontext(&cleanUpCtx);
	cleanUpCtx.uc_stack.ss_sp = (taskNode_t*) calloc(16384, sizeof(char));\
	cleanUpCtx.uc_stack.ss_size = 16384 * sizeof(char);
	cleanUpCtx.uc_link = &(mainTask->context);
	makecontext(&cleanUpCtx, cleanUpFunc, 0);

	struct itimerval new;
	new.it_interval.tv_usec = 0;
	new.it_interval.tv_sec = 1;
	new.it_value.tv_usec = 0;
	new.it_value.tv_sec = 1;
	setitimer (ITIMER_REAL, &new, NULL);

}

void cleanUpFunc(void) {
	while (1) {
		listRemove(&tSchedListHead, currTask);
		currTask->tState = ZOMBIE;
		//current task has ended
		currTask = NULL;
		currTask = getNextTask();
		swapcontext(&cleanUpCtx, &(currTask->context));
	}
}

void blockSched(void) {
	sigset_t mask;
	sigemptyset (&mask);
	sigaddset (&mask, SIGALRM); 
	sigprocmask(SIG_BLOCK, &mask, NULL);
}

void unblockSched(void) {
	sigset_t mask;
	sigemptyset (&mask);
	sigaddset (&mask, SIGALRM); 
	sigprocmask(SIG_UNBLOCK, &mask, NULL);
}

taskNode_t *createTask(void) {
	taskNode_t *newTask = (taskNode_t*) calloc(1, sizeof(taskNode_t));
	if (newTask) {
		newTask->tState = ALLOC;
	}
	return newTask;
}

/*
* macro is used to be able to pass VA_ARGS to makecontext from calling initTask
* other approach would be in-line assembly to push vargs to stack before makecontext call
*/
#define initTask(newTask, func, argc, ...)									\
	do {																	\
		taskNode_t *oldTask;												\
		if (!newTask || newTask->tState != ALLOC) break;					\
		getcontext(&(newTask->context));									\
		newTask->context.uc_stack.ss_sp = (taskNode_t*) calloc(16384, sizeof(char));\
		newTask->context.uc_stack.ss_size = 16384 * sizeof(char);			\
		newTask->context.uc_link = &cleanUpCtx;								\
		makecontext(&(newTask->context), func, argc, ##__VA_ARGS__);		\
		newTask->tState = READY;												\
		listAdd(&tSchedListHead, newTask);									\
		schedule();															\
	} while (0);

void schedule(void) {
	taskNode_t *oldTask = switchTasks();
#ifdef DEBUG
	printf("schedule\n");
#endif
	swapcontext(&(oldTask->context), &(currTask->context));
}

//scheduling algorithm goes here, now it is simple round robin
taskNode_t *getNextTask(void) {
	taskNode_t *nextTask;
	if (currTask == NULL) {
		nextTask = &tSchedListHead;
	}
	else {
		nextTask = currTask;
	}
	do {
		nextTask = listGetNext(&tSchedListHead, nextTask);
	} while (nextTask->tState != READY && nextTask->tState != RUNNING);
	return nextTask;
}

taskNode_t *switchTasks(void) {
	taskNode_t *oldTask = currTask;
	currTask = getNextTask();
	oldTask->tState = READY;
	currTask->tState = RUNNING;
	return oldTask;
}

void taskJoin(const taskNode_t *tWait) {
	if (tWait) {
		while (tWait->tState != ZOMBIE) {
			schedule();
		}
	}
}

void setCtx(ucontext_t *destCtx, ucontext_t *srcCtx) {
	//18 because we cannot change REG_CSGSFS(19) - kernel causes crash of our app
	memcpy(destCtx->uc_mcontext.gregs, srcCtx->uc_mcontext.gregs, 18 * sizeof(greg_t));
	memcpy(destCtx->uc_mcontext.fpregs, srcCtx->uc_mcontext.fpregs, sizeof(struct _libc_fpstate));
	destCtx->uc_stack.ss_sp = srcCtx->uc_stack.ss_sp;
	destCtx->uc_stack.ss_size = srcCtx->uc_stack.ss_size;
	destCtx->uc_sigmask = srcCtx->uc_sigmask;
	destCtx->uc_flags = srcCtx->uc_flags;
	destCtx->uc_link = srcCtx->uc_link;
}

static void sigHand (int sig, siginfo_t *siginfo, void *vcontext) {
	ucontext_t *curContext = (ucontext_t*) vcontext;
#ifdef DEBUG
	printf("signal handle\n");
#endif
	//save current task context
	setCtx(&(currTask->context), curContext);
	switchTasks();
	setCtx(curContext, &(currTask->context));
}

/**********************************/
/* User functions */

static void func1(void) {
	int i = 0;
	while (i < 10) {
		printf("func1 loop %i\n", i);
		++i;
		usleep(500000);
	}
}

static void func2(void) {
	int i = 0;
	while (1) {
		printf("func2 loop %i\n", i);
		++i;
		usleep(500000);
	}
}

static void func3(void) {
	int i = 0;
	while (1) {
		printf("func3 loop %i\n", i);
		++i;
		usleep(500000);
	}
}

main() {
	taskLibInit();

	taskNode_t *new = createTask();
	initTask(new, func1, 0);
	taskNode_t *new2 = createTask();
	initTask(new2, func2, 0);
	taskNode_t *new3 = createTask();
	initTask(new3, func3, 0);

	while (1) {
		printf("main loop\n");
		usleep(500000);
	}
	return 0;
}
