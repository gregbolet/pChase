/*******************************************************************************
 * Copyright (c) 2006 International Business Machines Corporation.             *
 * All rights reserved. This program and the accompanying materials            *
 * are made available under the terms of the Common Public License v1.0        *
 * which accompanies this distribution, and is available at                    *
 * http://www.opensource.org/licenses/cpl1.0.php                               *
 *                                                                             *
 * Contributors:                                                               *
 *    Douglas M. Pase - initial API and implementation                         *
 *    Tim Besard - prefetching, JIT compilation                                *
 *******************************************************************************/

//
// Configuration
//

// Implementation header
#include "thread.h"

// System includes
#include <cstdio>
#include <pthread.h>
#include <unistd.h>

#ifdef GREG
#include <stdio.h>
#endif

Lock Thread::_global_lock;
int Thread::count = 0;


//
// Implementation
//

Thread::Thread() {
	Thread::global_lock();
	this->id = Thread::count;
	Thread::count += 1;
	Thread::global_unlock();
}

Thread::~Thread() {
}

int Thread::start() {
	return pthread_create(&this->thread, NULL, Thread::start_routine, this);
}

void*
Thread::start_routine(void* p) {
	// get the current affinity
	cpu_set_t cs; // CPU set structure
	CPU_ZERO(&cs); //Clear the set to contain no CPUs
	sched_getaffinity(0, sizeof(cs), &cs); //get the affinity of the calling process (i.e: this thread)

	// deduce the amount of CPUs
	// on our test system we have 8 cores, so this value
	// should end up being 8 'CPUs'
	int count = 0;
	for (; CPU_ISSET(count, &cs); count++);

#ifdef GREG
	printf("GREG NUM CPUS '%d'\n", count);
#endif

	// restrict to a single CPU (i.e: core)
	CPU_ZERO(&cs); // clear the set so it contains no CPUs
	size_t size = CPU_ALLOC_SIZE(1); // get the size of a cpuset containing 1 CPU

	// Cyclically distribute the threads among cores (using mod)
	// i.e: Spread scheduling
	CPU_SET_S(((Thread*) p)->id % count, size, &cs); // Add the core (remainder value) to the set
	pthread_setaffinity_np(pthread_self(), size, &cs); // Set the pthread affinity to the single core

	// run
	((Thread*) p)->run();

	return NULL;
}

void Thread::exit() {
	pthread_exit(NULL);
}

int Thread::wait() {
	pthread_join(this->thread, NULL);

	return 0;
}

void Thread::lock() {
	this->object_lock.lock();
}

void Thread::unlock() {
	this->object_lock.unlock();
}

void Thread::global_lock() {
	Thread::_global_lock.lock();
}

void Thread::global_unlock() {
	Thread::_global_lock.unlock();
}
