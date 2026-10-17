#include "util/ps4_semaphore.hpp"
#include <atomic>
#include <thread>
#include <vector>

int main()
{
	ps4_rpcn_semaphore ready(0), finished(0);
	std::atomic<unsigned> consumed{0};
	std::vector<std::thread> consumers;
	for (unsigned i = 0; i < 4; ++i)
	{
		consumers.emplace_back([&]
		{
			for (unsigned n = 0; n < 5000; ++n)
			{
				ready.acquire();
				++consumed;
			}
			finished.release();
		});
	}
	for (unsigned i = 0; i < 20000; ++i) ready.release();
	for (unsigned i = 0; i < 4; ++i)
		assert(finished.try_acquire_for(std::chrono::seconds(10)));
	for (auto& thread : consumers) thread.join();
	assert(consumed == 20000);
	assert(!ready.try_acquire_for(std::chrono::milliseconds(1)));
	ready.release(3);
	for (unsigned i = 0; i < 3; ++i)
		assert(ready.try_acquire_for(std::chrono::seconds(0)));
	assert(!ready.try_acquire_for(std::chrono::seconds(0)));
}
