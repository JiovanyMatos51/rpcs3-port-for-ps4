// Host test of utils::ps4::host_lock / host_wait (util/ps4_host_wait.hpp): the lv2-aware lock loop used by the HLE
// modules. Build (from the repo root):
//   clang++ -std=c++20 -static -Wall -Wextra -I rpcs3 ps4-port/tests/host_wait_test.cpp -o host_wait_test.exe
#include "util/ps4_host_wait.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

using utils::ps4::host_detach;
using utils::ps4::host_lock_path;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

namespace
{
	using namespace std::chrono_literals;

	// Scripted single-thread ops: counts every call and checks the ordering rules.
	struct scripted_ops
	{
		bool is_guest = true;
		bool nested = false;
		bool stop = false;
		unsigned free_after_tries = 0;     // try_lock succeeds from this try on (0: never)
		std::vector<host_detach> detaches;  // results of successive detach() calls (last one repeats)

		unsigned tries = 0, locks = 0, spins = 0, detach_calls = 0, attach_calls = 0, waits = 0;
		unsigned notes = 0, nested_notes = 0;
		bool owns = false;
		bool detached = false;

		bool try_lock()
		{
			tries++;
			if (free_after_tries && tries >= free_after_tries)
			{
				owns = true;
				return true;
			}
			return false;
		}

		void lock() { locks++; owns = true; }
		void spin() { spins++; }
		bool guest() const { return is_guest; }
		bool other_locks_held() const { return nested; }
		bool stopping() const { return stop; }

		host_detach detach()
		{
			CHECK(!owns);
			CHECK(!detached);
			const host_detach r = detaches.empty() ? host_detach::detached : detaches[std::min<std::size_t>(detach_calls, detaches.size() - 1)];
			detach_calls++;
			detached = r == host_detach::detached;
			return r;
		}

		void attach()
		{
			// Rule: never re-enter the run queue (which may park) while owning the lock
			CHECK(!owns);
			CHECK(detached);
			detached = false;
			attach_calls++;
		}

		void wait_free() { CHECK(!owns); waits++; }
		void note_wait() { notes++; }
		void note_nested() { nested_notes++; }
	};

	void test_uncontended()
	{
		scripted_ops ops;
		ops.free_after_tries = 1;
		CHECK(utils::ps4::host_lock(ops) == host_lock_path::uncontended);
		CHECK(ops.tries == 1 && ops.spins == 0 && ops.detach_calls == 0 && ops.attach_calls == 0 && ops.notes == 0 && ops.locks == 0);
	}

	void test_spun()
	{
		scripted_ops ops;
		ops.free_after_tries = 4;
		CHECK(utils::ps4::host_lock(ops) == host_lock_path::spun);
		CHECK(ops.spins == 3 && ops.detach_calls == 0 && ops.attach_calls == 0 && ops.notes == 0);
	}

	void test_host_thread()
	{
		scripted_ops ops;
		ops.is_guest = false;
		CHECK(utils::ps4::host_lock(ops) == host_lock_path::plain);
		CHECK(ops.locks == 1 && ops.spins == 0 && ops.detach_calls == 0 && ops.attach_calls == 0);
	}

	void test_nested()
	{
		scripted_ops ops;
		ops.nested = true;
		CHECK(utils::ps4::host_lock(ops) == host_lock_path::nested);
		CHECK(ops.locks == 1 && ops.spins == utils::ps4::host_lock_spins && ops.nested_notes == 1 && ops.notes == 0);
		CHECK(ops.detach_calls == 0 && ops.attach_calls == 0);
	}

	void test_stopping()
	{
		scripted_ops ops;
		ops.stop = true;
		CHECK(utils::ps4::host_lock(ops) == host_lock_path::stopping);
		CHECK(ops.locks == 1 && ops.detach_calls == 0 && ops.attach_calls == 0 && ops.notes == 1);
	}

	void test_waited_detached()
	{
		// 1 + 10 spin tries fail, the 3rd loop try succeeds: three detached waits, each attached before the next try
		scripted_ops ops;
		ops.free_after_tries = 1 + utils::ps4::host_lock_spins + 3;
		CHECK(utils::ps4::host_lock(ops) == host_lock_path::waited);
		CHECK(ops.detach_calls == 3 && ops.attach_calls == 3 && ops.waits == 3 && ops.notes == 1 && ops.locks == 0);
		CHECK(ops.owns && !ops.detached);
	}

	void test_not_queued()
	{
		// A thread already asleep in lv2 (not queued) waits without ever attaching
		scripted_ops ops;
		ops.free_after_tries = 1 + utils::ps4::host_lock_spins + 2;
		ops.detaches = {host_detach::not_queued};
		CHECK(utils::ps4::host_lock(ops) == host_lock_path::waited);
		CHECK(ops.detach_calls == 2 && ops.attach_calls == 0 && ops.waits == 2);
	}

	void test_signaled_race()
	{
		// A wake-up raced the first detach: detach again, then wait detached and attach once
		scripted_ops ops;
		ops.free_after_tries = 1 + utils::ps4::host_lock_spins + 1;
		ops.detaches = {host_detach::signaled, host_detach::detached};
		CHECK(utils::ps4::host_lock(ops) == host_lock_path::waited);
		CHECK(ops.detach_calls == 2 && ops.attach_calls == 1 && ops.waits == 1);

		// A wake-up that keeps racing ends in an attached wait (bounded retries), never in an attach
		scripted_ops stuck;
		stuck.detaches = {host_detach::signaled};
		unsigned waited = 0;
		CHECK(utils::ps4::host_wait(stuck, [&] { waited++; }) == host_detach::signaled);
		CHECK(waited == 1 && stuck.attach_calls == 0 && stuck.detach_calls == 1 + utils::ps4::host_detach_retries);
	}

	void test_host_wait_order()
	{
		struct order_ops
		{
			std::vector<char> calls;
			host_detach r = host_detach::detached;
			host_detach detach() { calls.push_back('d'); return r; }
			void attach() { calls.push_back('a'); }
		};

		order_ops ops;
		CHECK(utils::ps4::host_wait(ops, [&] { ops.calls.push_back('w'); }) == host_detach::detached);
		CHECK((ops.calls == std::vector<char>{'d', 'w', 'a'}));

		order_ops asleep;
		asleep.r = host_detach::not_queued;
		CHECK(utils::ps4::host_wait(asleep, [&] { asleep.calls.push_back('w'); }) == host_detach::not_queued);
		CHECK((asleep.calls == std::vector<char>{'d', 'w'}));
	}

	// Mock lv2 scheduler with one PPU slot: a thread runs guest/HLE code only while it holds a slot. A thread blocked
	// inside the slot (attached) keeps it, which is the Mortal Kombat freeze: the lock holder was preempted and needs
	// the slot back to release the lock, and the waiter never gives the slot up.
	struct mock_scheduler
	{
		std::mutex m;
		std::condition_variable cv;
		int owner = 0; // thread id holding the single slot, 0 = free

		bool acquire(int id, std::chrono::milliseconds timeout)
		{
			std::unique_lock lock(m);
			if (!cv.wait_for(lock, timeout, [&] { return owner == 0 || owner == id; }))
			{
				return false;
			}
			owner = id;
			return true;
		}

		void release(int id)
		{
			std::lock_guard lock(m);
			if (owner == id)
			{
				owner = 0;
			}
			cv.notify_all();
		}
	};

	// The host lock (an HLE module mutex) with lock_unlock-like waiting
	struct mock_mutex
	{
		std::mutex m;
		std::condition_variable cv;
		bool locked = false;

		bool try_lock()
		{
			std::lock_guard lock(m);
			if (locked) return false;
			locked = true;
			return true;
		}

		void lock()
		{
			std::unique_lock lock(m);
			cv.wait(lock, [&] { return !locked; });
			locked = true;
		}

		void unlock()
		{
			std::lock_guard lock(m);
			locked = false;
			cv.notify_all();
		}

		void wait_free()
		{
			std::unique_lock lock(m);
			cv.wait(lock, [&] { return !locked; });
		}
	};

	// Runs the MK cycle once; returns true if the holder could resume (no freeze)
	bool run_mk_cycle(bool use_helper)
	{
		mock_scheduler sched;
		mock_mutex audio_mutex;
		std::atomic<bool> holder_locked{false};
		std::atomic<bool> waiter_has_slot{false};
		std::atomic<bool> holder_resumed{false};
		std::atomic<bool> waiter_done{false};
		std::atomic<bool> attach_while_owning{false};
		std::atomic<bool> waiter_owns{false};

		constexpr int holder_id = 1;
		constexpr int waiter_id = 2;

		// Holder (PoolThread in cellAudioPortClose): takes the lock, then lv2 parks it (slot lost) inside the lock
		std::thread holder([&]
		{
			CHECK(sched.acquire(holder_id, 1000ms));
			CHECK(audio_mutex.try_lock());
			holder_locked = true;
			sched.release(holder_id); // parked by lv2 with the mutex held

			// The waiter runs now and blocks on the mutex
			while (!waiter_has_slot) std::this_thread::yield();
			std::this_thread::sleep_for(20ms);

			// Resume needs the slot back
			holder_resumed = sched.acquire(holder_id, 500ms);

			// Break the cycle either way so the test ends
			audio_mutex.unlock();

			if (holder_resumed)
			{
				sched.release(holder_id);
			}
		});

		// Waiter (_cellsurMixerMain in cellAudioGetPortBlockTag)
		std::thread waiter([&]
		{
			while (!holder_locked) std::this_thread::yield();
			CHECK(sched.acquire(waiter_id, 1000ms));
			waiter_has_slot = true;

			if (!use_helper)
			{
				audio_mutex.lock(); // blocks attached: keeps the slot
				waiter_owns = true;
			}
			else
			{
				struct ops_t
				{
					mock_scheduler& sched;
					mock_mutex& mtx;
					std::atomic<bool>& owns;
					std::atomic<bool>& bad;
					bool detached = false;

					bool try_lock() { const bool ok = mtx.try_lock(); if (ok) owns = true; return ok; }
					void lock() { mtx.lock(); owns = true; }
					void spin() { std::this_thread::yield(); }
					bool guest() const { return true; }
					bool other_locks_held() const { return false; }
					bool stopping() const { return false; }
					host_detach detach() { sched.release(waiter_id); detached = true; return host_detach::detached; }
					void attach()
					{
						if (owns) bad = true;
						CHECK(sched.acquire(waiter_id, 2000ms));
						detached = false;
					}
					void wait_free() { mtx.wait_free(); }
					void note_wait() {}
					void note_nested() {}
				} ops{sched, audio_mutex, waiter_owns, attach_while_owning};

				CHECK(utils::ps4::host_lock(ops) == host_lock_path::waited);
				CHECK(!ops.detached);
			}

			// Holding the lock and the slot: the HLE call finishes
			audio_mutex.unlock();
			waiter_owns = false;
			sched.release(waiter_id);
			waiter_done = true;
		});

		holder.join();
		waiter.join();
		CHECK(waiter_done);
		CHECK(!attach_while_owning);
		return holder_resumed;
	}

	// Several PPU threads hammer one lock through the helper with two slots and a holder that parks inside the lock:
	// every acquisition completes and no attach happens while owning the lock.
	void test_stress()
	{
		mock_scheduler slots[2];
		mock_mutex mtx;
		std::atomic<unsigned> done{0};
		std::atomic<bool> bad{false};
		std::atomic<int> inside{0};

		// Two-slot scheduler built from two single-slot ones
		auto take_slot = [&](int id) -> int
		{
			for (auto until = std::chrono::steady_clock::now() + 5s; std::chrono::steady_clock::now() < until;)
			{
				for (int s = 0; s < 2; s++)
				{
					if (slots[s].acquire(id, 1ms)) return s;
				}
			}
			return -1;
		};

		constexpr int threads = 6;
		constexpr int rounds = 300;
		std::vector<std::thread> pool;

		for (int t = 1; t <= threads; t++)
		{
			pool.emplace_back([&, t]
			{
				int slot = take_slot(t);
				CHECK(slot >= 0);

				for (int r = 0; r < rounds; r++)
				{
					struct ops_t
					{
						int id;
						int& slot;
						std::function<int(int)>* take;
						mock_scheduler* slots;
						mock_mutex& m;
						std::atomic<bool>& bad;
						bool owns = false;

						bool try_lock() { owns = m.try_lock(); return owns; }
						void lock() { m.lock(); owns = true; }
						void spin() { std::this_thread::yield(); }
						bool guest() const { return true; }
						bool other_locks_held() const { return false; }
						bool stopping() const { return false; }
						host_detach detach() { slots[slot].release(id); slot = -1; return host_detach::detached; }
						void attach() { if (owns) bad = true; slot = (*take)(id); CHECK(slot >= 0); }
						void wait_free() { m.wait_free(); }
						void note_wait() {}
						void note_nested() {}
					};

					std::function<int(int)> take = take_slot;
					ops_t ops{t, slot, &take, slots, mtx, bad};
					utils::ps4::host_lock(ops);
					CHECK(++inside == 1);

					if (r % 7 == 0)
					{
						// Parked by lv2 inside the lock: slot lost, needs it back to release
						slots[slot].release(t);
						slot = -1;
						std::this_thread::sleep_for(100us);
						slot = take_slot(t);
						CHECK(slot >= 0);
					}

					CHECK(--inside == 0);
					mtx.unlock();
				}

				slots[slot].release(t);
				done++;
			});
		}

		for (auto& th : pool) th.join();
		CHECK(done == threads);
		CHECK(!bad);
	}
}

int main()
{
	test_uncontended();
	test_spun();
	test_host_thread();
	test_nested();
	test_stopping();
	test_waited_detached();
	test_not_queued();
	test_signaled_race();
	test_host_wait_order();

	// The MK freeze: a plain lock inside the run queue never lets the parked holder resume; the helper does
	CHECK(!run_mk_cycle(false));
	CHECK(run_mk_cycle(true));

	test_stress();

	std::puts("host_wait_test: all tests passed");
	return 0;
}
