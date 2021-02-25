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
#include "run.h"

// System includes
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <cstddef>
#include <algorithm>
#if defined(NUMA)
#include <numa.h>
#endif

// Local includes
#include <AsmJit/AsmJit.h>
#include "timer.h"

#ifdef GREG
#include <stdio.h>
#endif


//
// Implementation
//

typedef void (*benchmark)(const Chain**);
typedef benchmark (*generator)(int64 chains_per_thread,
		int64 bytes_per_line, int64 bytes_per_chain,
		int64 stride, int64 loop_length, int32 prefetch_hint);
static benchmark chase_pointers(int64 chains_per_thread,
		int64 bytes_per_line, int64 bytes_per_chain,
		int64 stride, int64 loop_length, int32 prefetch_hint);

Lock Run::global_mutex;
int64 Run::_ops_per_chain = 0;
std::vector<double> Run::_seconds;

Run::Run() :
		exp(NULL), bp(NULL) {
}

Run::~Run() {
}

void Run::set(Experiment &e, SpinBarrier* sbp) {
	this->exp = &e;
	this->bp = sbp;
}

// Each thread will run this function
int Run::run() {

#ifdef PERF_CNTR_MODE
	// Keep track of the eventset identifier for this thread
 	int EventSet = PAPI_NULL;
 	int retval;

	// Setup the event counter memory where we will store
	// the results of ALL the experiments on this thread
	// Let's initialize the whole array to 0
 	this->cntr_values = (long long*) calloc(NUMEVENTS * this->exp->experiments, sizeof(long long));

	// Register this thread with PAPI
	if ( ( retval = PAPI_register_thread() ) != PAPI_OK ) {
		fprintf(stderr, "PAPI thread registration error!\n");
	}

	/* Create the Event Set for this thread */
	if (PAPI_create_eventset(&EventSet) != PAPI_OK){
		fprintf(stderr, "PAPI eventset creation error!\n");
	}

	/* In Component PAPI, EventSets must be assigned a component index
	   before you can fiddle with their internals. 0 is always the cpu component */
	retval = PAPI_assign_eventset_component( EventSet, 0 );
	if ( retval != PAPI_OK ) {
		fprintf(stderr, "PAPI assign eventset component error!\n");
	}

	/* Convert our EventSet to a multiplexed EventSet*/
	if ( ( retval = PAPI_set_multiplex( EventSet ) ) != PAPI_OK ) {
		if ( retval == PAPI_ENOSUPP) {
			fprintf(stderr, "PAPI Multiplexing not supported!\n");
	   	}
		fprintf(stderr, "PAPI error setting up multiplexing!\n");
	}

	// Add each of the events by getting their event code from the string
	int eventCode = PAPI_NULL;
	for(int i = 0; i < NUMEVENTS; i++){

		if((retval = PAPI_event_name_to_code(Experiment::events_to_track[i], &eventCode)) != PAPI_OK){
			fprintf(stderr, "Event code for [%s] does not exist! errcode:[%d]\n", 
				Experiment::events_to_track[i], retval);
		}

		/* Add the events to track to out EventSet */
		if ((retval=PAPI_add_event(EventSet, eventCode)) != PAPI_OK){
			fprintf(stderr, "Add event [%s] error! errcode:[%d]\n", 
				Experiment::events_to_track[i], retval);
		}
	}

	// Make sure all the threads get here
	this->bp->barrier();
#endif

	// first allocate all memory for the chains,
	// making sure it is allocated within the
	// intended numa domains
	Chain** chain_memory = new Chain*[this->exp->chains_per_thread];
	Chain** root = new Chain*[this->exp->chains_per_thread];

#if defined(NUMA)
	// establish the node id where this thread
	// will run. threads are mapped to nodes
	// by the set-up code for Experiment.
	int run_node_id = this->exp->thread_domain[this->thread_id()];
	numa_run_on_node(run_node_id);

	// establish the node id where this thread's
	// memory will be allocated.
	for (int i=0; i < this->exp->chains_per_thread; i++) {
		int alloc_node_id = this->exp->chain_domain[this->thread_id()][i];
		nodemask_t alloc_mask;
		nodemask_zero(&alloc_mask);
		nodemask_set(&alloc_mask, alloc_node_id);
		numa_set_membind(&alloc_mask);

		chain_memory[i] = new Chain[ this->exp->links_per_chain ];
	}
#else
	for (int i = 0; i < this->exp->chains_per_thread; i++) {
		chain_memory[i] = new Chain[this->exp->links_per_chain];
	}
#endif

	// initialize the chains and
	// select the function that
	// will generate the tests
	generator gen;
	for (int i = 0; i < this->exp->chains_per_thread; i++) {
		if (this->exp->access_pattern == Experiment::RANDOM) {
			root[i] = random_mem_init(chain_memory[i]);
			gen = chase_pointers;
		} else if (this->exp->access_pattern == Experiment::STRIDED) {
			if (0 < this->exp->stride) {
				root[i] = forward_mem_init(chain_memory[i]);
			} else {
				root[i] = reverse_mem_init(chain_memory[i]);
			}
			gen = chase_pointers;
		}
	}

	// compile benchmark
	benchmark bench = gen(this->exp->chains_per_thread,
			this->exp->bytes_per_line, this->exp->bytes_per_chain,
			this->exp->stride, this->exp->loop_length,
			this->exp->prefetch_hint);

	// calculate the number of iterations
	/*
	 * As soon as the thread count rises, this calculation HUGELY
	 * differs between runs. What does cause this? Threads are already
	 * limited to certain CPUs, so it's not caused by excessive switching.
	 * Strange cache behaviour?
	 */
	if (0 == this->exp->iterations) {
		volatile static double istart = 0;
		volatile static double istop = 0;

		// Stack variable, how do threads break out of loop?
		// Only thread 0 is updating elapsed
		volatile static double elapsed = 0; 
		volatile static int64 iters = 1;

		// Bound should always be 0.2. Why design it so?
		// Timer::resolution() will always have a difference of value
		// between Timer::seconds() at the 1E6 level, which is << 0.2 = 200 ms
		volatile double bound = std::max(0.2, 10 * Timer::resolution());

		// If the time elapsed is greater than 200 ms, then we have
		// the desired number of iterations
		for (iters = 1; elapsed <= bound; iters = iters << 1) {
			// barrier
			this->bp->barrier();

			// start timer
			if (this->thread_id() == 0) {
				istart = Timer::seconds();
			}
			this->bp->barrier();

			// chase pointers
#ifndef ITERSFIX
			for (int i = 0; i < iters; i++)
				bench((const Chain**) root);
#else
			for (int64 i = 0; i < iters; i++)
				bench((const Chain**) root);
#endif

			// barrier
			this->bp->barrier();

			// stop timer
			if (this->thread_id() == 0) {
				istop = Timer::seconds();
				elapsed = istop - istart;
			}
			this->bp->barrier();
		}

		// calculate the number of iterations
		if (this->thread_id() == 0) {
			// Default branch, exp->seconds is defaulted to 1 on exp init
			if (0 < this->exp->seconds) {
				// Execute up to a number of iterations such that we run for 1/2 a second
				this->exp->iterations = std::max(1.0,
						0.9999 + 0.5 * this->exp->seconds * iters / elapsed);
			} else {
				this->exp->iterations = std::max(1.0, 0.9999 + iters / elapsed);
			}
			//printf("Tested %d iterations: took %f seconds; scheduling %d iterations\n", iters, elapsed, this->exp->iterations);
		}
		this->bp->barrier();
	}

#ifdef ITERSTUDY
	// Have the main thread print this
	if(this->thread_id() == 0){
		printf("%lld, %d\n", this->exp->iterations, this->exp->iterations);
	}
	// Skip the run tests, only care about seeing the iterations counts
	goto SKIPRUNS;
#endif

#ifdef GREG
	printf("GREG Number of iterations: '%d'\n", this->exp->iterations);
#endif

	// run the experiments
	for (int e = 0; e < this->exp->experiments; e++) {
		// barrier
		this->bp->barrier();

		// start timer
		double start = 0;
		if (this->thread_id() == 0)
			start = Timer::seconds();
		this->bp->barrier();

#ifdef PERF_CNTR_MODE
		// Zero-out all the counters in the eventset
		if (PAPI_reset(EventSet) != PAPI_OK){
			fprintf(stderr, "Could NOT reset eventset!\n");
		}

		/* Start counting events in the Event Set */
		if (PAPI_start(EventSet) != PAPI_OK){
			fprintf(stderr, "Could NOT start eventset counting!\n");
		}
#endif

		// chase pointers
#ifndef ITERSFIX
		for (int i = 0; i < this->exp->iterations; i++)
			bench((const Chain**) root);
#else
		for (int64 i = 0; i < this->exp->iterations; i++)
			bench((const Chain**) root);
#endif

#ifdef PERF_CNTR_MODE
		/* stop counting events in the Event Set */
		// Store the resulting values into our counter values array
		if (PAPI_stop( EventSet, (this->cntr_values+(NUMEVENTS*e)) ) != PAPI_OK){
			fprintf(stderr, "Could NOT stop eventset counting!\n");
		}
#endif

		// barrier
		this->bp->barrier();

		// stop timer
		double stop = 0;
		if (this->thread_id() == 0)
			stop = Timer::seconds();
		this->bp->barrier();

		if (0 <= e) {
			if (this->thread_id() == 0) {
				double delta = stop - start;
				if (0 < delta) {
					Run::_seconds.push_back(delta);
				}
			}
		}
	}

#ifdef ITERSTUDY
SKIPRUNS:
#endif

	this->bp->barrier();

	// clean the memory
	for (int i = 0; i < this->exp->chains_per_thread; i++) {
		if (chain_memory[i] != NULL
			) delete[] chain_memory[i];
	}
	if (chain_memory != NULL
		) delete[] chain_memory;

#ifdef PERF_CNTR_MODE
	// Remove all events from the eventset
	if ( ( retval = PAPI_cleanup_eventset( EventSet ) ) != PAPI_OK ) {
		fprintf(stderr, "PAPI could not cleanup eventset!\n");
	}

	// Deallocate the empty eventset from memory
	if ( ( retval = PAPI_destroy_eventset( &EventSet) ) != PAPI_OK ) {
		fprintf(stderr, "PAPI could not destroy eventset!\n");
	}

	// Shut down this thread and free the thread ID
	if ( ( retval = PAPI_unregister_thread(  ) ) != PAPI_OK ) {
		fprintf(stderr, "PAPI could not unregister thread!\n");
	}

	// Store the counter values into the Experiments array
	Experiment::all_cntr_values[this->thread_id()] = this->cntr_values;

	// Free the counter values malloc we made at the beginning of the function
	// Free the counter values in the experiment class destructor
	//free(cntr_values);
#endif

	return 0;
}

int dummy = 0;
void Run::mem_check(Chain *m) {
	if (m == NULL
		) dummy += 1;
}

// exclude 2 and Mersenne primes, i.e.,
// primes of the form 2**n - 1, e.g.,
// 3, 7, 31, 127
static const int prime_table[] = { 5, 11, 13, 17, 19, 23, 37, 41, 43, 47, 53,
		61, 71, 73, 79, 83, 89, 97, 101, 103, 109, 113, 131, 137, 139, 149, 151,
		157, 163, };
static const int prime_table_size = sizeof prime_table / sizeof prime_table[0];

Chain*
Run::random_mem_init(Chain *mem) {
	// initialize pointers --
	// choose a page at random, then use
	// one pointer from each cache line
	// within the page.  all pages and
	// cache lines are chosen at random.
	Chain* root = 0;
	Chain* prev = 0;
	int link_within_line = 0;
	int64 local_ops_per_chain = 0;

	// we must set a lock because random()
	// is not thread safe
	Run::global_mutex.lock();
	setstate(this->exp->random_state[this->thread_id()]);
	int page_factor = prime_table[random() % prime_table_size];
	int page_offset = random() % this->exp->pages_per_chain;
	Run::global_mutex.unlock();

	// loop through the pages
	for (int i = 0; i < this->exp->pages_per_chain; i++) {
		int page = (page_factor * i + page_offset) % this->exp->pages_per_chain;
		Run::global_mutex.lock();
		setstate(this->exp->random_state[this->thread_id()]);
		int line_factor = prime_table[random() % prime_table_size];
		int line_offset = random() % this->exp->lines_per_page;
		Run::global_mutex.unlock();

		// loop through the lines within a page
		for (int j = 0; j < this->exp->lines_per_page; j++) {
			int line_within_page = (line_factor * j + line_offset)
					% this->exp->lines_per_page;
			int link = page * this->exp->links_per_page
					+ line_within_page * this->exp->links_per_line
					+ link_within_line;

			if (root == 0) {
				prev = root = mem + link;
				local_ops_per_chain += 1;
			} else {
				prev->next = mem + link;
				prev = prev->next;
				local_ops_per_chain += 1;
			}
		}
	}

	prev->next = root;

	Run::global_mutex.lock();
	Run::_ops_per_chain = local_ops_per_chain;
	Run::global_mutex.unlock();

	return root;
}

Chain*
Run::forward_mem_init(Chain *mem) {
	Chain* root = 0;
	Chain* prev = 0;
	int link_within_line = 0;
	int64 local_ops_per_chain = 0;

	for (int i = 0; i < this->exp->lines_per_chain; i += this->exp->stride) {
		int link = i * this->exp->links_per_line + link_within_line;
		if (root == NULL) {
			prev = root = mem + link;
			local_ops_per_chain += 1;
		} else {
			prev->next = mem + link;
			prev = prev->next;
			local_ops_per_chain += 1;
		}
	}

	prev->next = root;

	Run::global_mutex.lock();
	Run::_ops_per_chain = local_ops_per_chain;
	Run::global_mutex.unlock();

	return root;
}

Chain*
Run::reverse_mem_init(Chain *mem) {
	Chain* root = 0;
	Chain* prev = 0;
	int link_within_line = 0;
	int64 local_ops_per_chain = 0;

	int stride = -this->exp->stride;
	int last;
	for (int i = 0; i < this->exp->lines_per_chain; i += stride) {
		last = i;
	}

	for (int i = last; 0 <= i; i -= stride) {
		int link = i * this->exp->links_per_line + link_within_line;
		if (root == 0) {
			prev = root = mem + link;
			local_ops_per_chain += 1;
		} else {
			prev->next = mem + link;
			prev = prev->next;
			local_ops_per_chain += 1;
		}
	}

	prev->next = root;

	Run::global_mutex.lock();
	Run::_ops_per_chain = local_ops_per_chain;
	Run::global_mutex.unlock();

	return root;
}

static benchmark chase_pointers(int64 chains_per_thread, // memory loading per thread
		int64 bytes_per_line, // ignored
		int64 bytes_per_chain, // ignored
		int64 stride, // ignored
		int64 loop_length, // length of the inner loop
		int32 prefetch_hint // use of prefetching
		) {
	// Create Compiler.
	AsmJit::Compiler c;

  	// Tell compiler the function prototype we want. It allocates variables representing
	// function arguments that can be accessed through Compiler or Function instance.
	c.newFunction(AsmJit::CALL_CONV_DEFAULT, AsmJit::FunctionBuilder1<AsmJit::Void, const Chain**>());

	// Try to generate function without prolog/epilog code:
	c.getFunction()->setHint(AsmJit::FUNCTION_HINT_NAKED, true);

	// Create labels.
	AsmJit::Label L_Loop = c.newLabel();

	// Function arguments.
	AsmJit::GPVar chain(c.argGP(0));

	// Save the head
	std::vector<AsmJit::GPVar> heads(chains_per_thread);
	for (int i = 0; i < chains_per_thread; i++) {
		AsmJit::GPVar head = c.newGP();
		c.mov(head, ptr( chain, i * sizeof( Chain * ) ) );
		heads[i] = head;
	}

	// Current position
	std::vector<AsmJit::GPVar> positions(chains_per_thread);
	for (int i = 0; i < chains_per_thread; i++) {
		AsmJit::GPVar position = c.newGP();
		c.mov(position, heads[i]);
		positions[i] = position;
	}

	// Loop.
	c.bind(L_Loop);

	// Process all links
	for (int i = 0; i < chains_per_thread; i++) {
		// Chase pointer
		c.mov(positions[i], ptr(positions[i], offsetof(Chain, next)));

		// Prefetch next
		switch (prefetch_hint)
		{
		case Experiment::T0:
			c.prefetch(ptr(positions[i]), AsmJit::PREFETCH_T0);
			break;
		case Experiment::T1:
			c.prefetch(ptr(positions[i]), AsmJit::PREFETCH_T1);
			break;
		case Experiment::T2:
			c.prefetch(ptr(positions[i]), AsmJit::PREFETCH_T2);
			break;
		case Experiment::NTA:
			c.prefetch(ptr(positions[i]), AsmJit::PREFETCH_NTA);
			break;
		case Experiment::NONE:
		default:
			break;

		}
	}

	// Wait
	for (int i = 0; i < loop_length; i++)
		c.nop();

	// Test if end reached
	c.cmp(heads[0], positions[0]);
	c.jne(L_Loop);

	// Finish.
	c.endFunction();

	// Make JIT function.
	benchmark fn = AsmJit::function_cast<benchmark>(c.make());

	// Ensure that everything is ok.
	if (!fn) {
		printf("Error making jit function (%u).\n", c.getError());
		return 0;
	}

	return fn;
}
