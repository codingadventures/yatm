/*
** MIT License
**
** Copyright(c) 2019, Pantelis Lekakis
**
** Permission is hereby granted, free of charge, to any person obtaining a copy
** of this software and associated documentation files(the "Software"), to deal
** in the Software without restriction, including without limitation the rights
** to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
** copies of the Software, and to permit persons to whom the Software is
** furnished to do so, subject to the following conditions :
**
** The above copyright notice and this permission notice shall be included in all
** copies or substantial portions of the Software.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
** AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
** LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
** OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
** SOFTWARE.
*/

#include <iostream>

// Selecting which sample to run.
#define YATM_SAMPLE_PARALLEL_FOR		(0)
#define YATM_SAMPLE_JOB_DEPENDENCIES	(1)

#define YATM_SAMPLE YATM_SAMPLE_PARALLEL_FOR

#define COUNT_OF(x) sizeof((x)) / sizeof((x)[0])

#ifdef _MSC_VER
#define sprintf_fn sprintf_s
#else
#define sprintf_fn sprintf
#endif //_MSVC

#define YATM_DEBUG (_DEBUG)
//#define YATM_WIN64 (1u)
#define YATM_STD_THREAD (1u)
#include "../include/yatm.hpp"

#include <chrono>
using clk = std::chrono::high_resolution_clock;
using ms = std::chrono::milliseconds;
using fsec = std::chrono::duration<float>;

#define RUN_SINGLETHREADED (0u)

// -----------------------------------------------------------------------------------------------
class scoped_profiler
{
public:
	// -----------------------------------------------------------------------------------------------
	scoped_profiler()
	{
		m_start = clk::now();

		std::cout << "BEGIN" << std::endl;
	}

	// -----------------------------------------------------------------------------------------------
	~scoped_profiler()
	{
		auto end = clk::now();
		fsec fs = end - m_start;
		ms d = std::chrono::duration_cast<ms>(fs);

		std::cout << "END (Elapsed: " << d.count() << "ms)" << std::endl;
	}

private:
	clk::time_point m_start;
};

// -----------------------------------------------------------------------------------------------
uint64_t work(uint32_t index)
{
	uint64_t result = 0ull;
	for (uint32_t x = 0; x < 5000; ++x)
	{
		for (uint32_t y = 0; y < 5000; ++y)
		{
			result += (y ^ (x + 10)) * (y - 1);
			result = (result << (index % 16));
			result = result >> (index / 2 % 8);
		}
		result |= x;
	}

	return result;
}

// -----------------------------------------------------------------------------------------------
void sample_parallel_for(yatm::scheduler& sch)
{
	while (true)
	{
		sch.reset();
		{
			scoped_profiler profiler;

			// Setup some data for processing
			const uint32_t dataLength = 100u;
			uint32_t uints[dataLength];

			for (uint32_t i = 0; i < COUNT_OF(uints); i++)
			{
				uints[i] = i;
			}

			// Launch them in parallel:
			// Creates as many tasks as the length of specified data, kicks them and blocks the caller thread until they are finished.
			sch.parallel_for((uint32_t*)uints, (uint32_t*)uints + dataLength, [](void* const param)
			{
				uint32_t idx = *(uint32_t*)param;

				// do some intensive work
				const auto result = work(idx);

				char t[64];
				sprintf_fn(t, "Result for data %u: %lld\n", idx, result);
				std::cout << t;
			});

			// An alternative way to specify functions, without lambdas.
			/*
			struct callback
			{
				void func(void* const param)
				{
					uint32_t idx = *(uint32_t*)param;

					// do some intensive work
					const auto result = work(idx);

					char t[64];
					sprintf_fn(t, "Result for data %u: %lld\n", idx, result);
					std::cout << t;
				}
			}owner;

			sch.parallel_for((uint32_t*)uints, (uint32_t*)uints + dataLength, yatm::bind(&callback::func, &owner));
			*/
		}
		sch.sleep(2000);
	}	
}

// -----------------------------------------------------------------------------------------------
void sample_job_dependencies(yatm::scheduler& sch)
{
	static uint32_t c_numChildTasks = 30;	// group child tasks.
	static uint32_t c_numIterations = ~0u;	// -1 for infinite iterations

	// Run for N iterations
	uint32_t iter = 0u;
	float averageMs = 0.0f;
	while ((iter++ < c_numIterations) || (c_numIterations == ~0u))
	{
		sch.reset();

		{
			scoped_profiler profiler;

			yatm::counter counter;

			// Prepare the job graph
			// This looks like this:
			/*
			[parent]
			/	     \
			/		  \
			[group0]	      [group1]
			/						\
			/						 \
			[group0_job]				  [group1_job]
			|							|
			|							|
			|---> child_0				|---> child_0
			| ....						| ...
			|---> child_n				|---> child_n

			Expected result is the children of each [groupN_job] task to be executed first. When all of the dependencies of each [groupN_job] are resolved,
			[groupN_job] will be executed. Once that happens, [groupN] is executed (being a simple group without a job function, it does nothing, simply used for grouping).
			Once both [group0] and [group1] are finished, [parent] executes and the tasks are complete.

			After [parent] is finished, sch.wait(parent) will unblock and main thread execution will continue.
			An alternative way to wait for the tasks to finish is by using the yatm::counter object. This is atomically incremented when jobs that reference it are added to
			the scheduler and decremented when jobs are finished. When the counter reached 0, it's assumed to be finished and sch.wait(&counter) will unblock the main thread.

			*/
			// Parent task depends on everything else below. This will be executed last.
			yatm::job* const parent = sch.create_job
			(
				[](void* const data)
				{
					std::cout << "Parent, this should execute after all the groups have finished.\n";
				}
				,
				nullptr,
				&counter
				);

			// allocate data for the child tasks; they simply hold the loop index, but more complex structures can be used.
			uint32_t* const data = sch.allocate<uint32_t>(c_numChildTasks, 16u);

			// Make a few groups to put the children jobs under. Group0 will depend on children [0, N/2-1] and group1 will depend on children [N/2, N]	
			// Group0_job and group1_job will execute once their respective children have finished executing.
			yatm::job* const group0 = sch.create_group(parent);
			yatm::job* const group0_job = sch.create_job([](void* const data) { std::cout << "Group 0 job, executing after all child 0 are finished.\n"; }, nullptr, &counter);
			sch.depend(group0, group0_job);

			yatm::job* const group1 = sch.create_group(parent);
			yatm::job* const group1_job = sch.create_job([](void* const data) { std::cout << "Group 1 job, executing after all child 1 are finished.\n"; }, nullptr, &counter);
			sch.depend(group1, group1_job);

			// Create child tasks
			for (uint32_t i = 0; i < c_numChildTasks; ++i)
			{
				data[i] = i;
				yatm::job* const child = sch.create_job
				(
					[](void* const data)
					{
						uint32_t idx = *(uint32_t*)data;

						// do some intensive work
						uint64_t result = work(idx);

						const uint32_t group = idx < c_numChildTasks / 2 ? 0 : 1;
						char str[512];
						sprintf_fn(str, "Child %u (group %u). Children of groups should execute first, result: %lld.\n", idx, group, result);

						std::cout << str;
					},
					&data[i],
					&counter
					);

				if (i < c_numChildTasks / 2)
				{
					sch.depend(group0_job, child);
				}
				else
				{
					sch.depend(group1_job, child);
				}
			}

			// Add the created tasks and signal the workers to begin processing them
			sch.kick();
			// Wait for finished tasks. Here we wait on the parent, as this will guarantee that all of the tasks will be complete.
			sch.wait(parent);

			// Or: 
			// sch.wait(&counter);
			//
			// The counter can also be only added on the parent (instead of all the tasks, as done above).
			// Since the parent depends on all the other tasks, having the counter only on that single job is enough.
		}
		// Pause for a bit, resume after 1000ms
		sch.set_paused(true);
		sch.sleep(1000);
		sch.set_paused(false);
	}
	sch.set_running(false);
	sch.sleep(2000);
}

// -----------------------------------------------------------------------------------------------
int main()
{
	yatm::scheduler sch;

	// Initialise the scheduler
	yatm::scheduler_desc desc;

#if RUN_SINGLETHREADED
	desc.m_numThreads = 1u;
#else
	desc.m_numThreads = sch.get_max_threads() - 1u;
#endif
	desc.m_jobScratchBufferInBytes = 4096u * 1024u;

	sch.init(desc);

#if (YATM_SAMPLE == YATM_SAMPLE_PARALLEL_FOR)
	sample_parallel_for(sch);
#elif (YATM_SAMPLE == YATM_SAMPLE_JOB_DEPENDENCIES)
	sample_job_dependencies(sch);
#endif

	return 0;
}

