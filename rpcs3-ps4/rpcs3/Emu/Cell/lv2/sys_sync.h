#pragma once

#include "Utilities/mutex.h"

#include "Emu/CPU/CPUThread.h"
#include "Emu/Cell/ErrorCodes.h"
#include "Emu/IdManager.h"
#include "Emu/IPC.h"

#include "util/shared_ptr.hpp"

#include <shared_mutex>
#include <type_traits>
#ifdef RPCS3_PS4
#include "util/ps4_host_wait.hpp"
#endif

// attr_protocol (waiting scheduling policy)
enum lv2_protocol : u8
{
	SYS_SYNC_FIFO                = 0x1, // First In, First Out Order
	SYS_SYNC_PRIORITY            = 0x2, // Priority Order
	SYS_SYNC_PRIORITY_INHERIT    = 0x3, // Basic Priority Inheritance Protocol
	SYS_SYNC_RETRY               = 0x4, // Not selected while unlocking
};

enum : u32
{
	SYS_SYNC_ATTR_PROTOCOL_MASK  = 0xf,
};

// attr_recursive (recursive locks policy)
enum
{
	SYS_SYNC_RECURSIVE           = 0x10,
	SYS_SYNC_NOT_RECURSIVE       = 0x20,
	SYS_SYNC_ATTR_RECURSIVE_MASK = 0xf0,
};

// attr_pshared (sharing among processes policy)
enum
{
	SYS_SYNC_PROCESS_SHARED      = 0x100,
	SYS_SYNC_NOT_PROCESS_SHARED  = 0x200,
	SYS_SYNC_ATTR_PSHARED_MASK   = 0xf00,
};

// attr_flags (creation policy)
enum
{
	SYS_SYNC_NEWLY_CREATED       = 0x1, // Create new object, fails if specified IPC key exists
	SYS_SYNC_NOT_CREATE          = 0x2, // Reference existing object, fails if IPC key not found
	SYS_SYNC_NOT_CARE            = 0x3, // Reference existing object, create new one if IPC key not found
	SYS_SYNC_ATTR_FLAGS_MASK     = 0xf,
};

// attr_adaptive
enum
{
	SYS_SYNC_ADAPTIVE            = 0x1000,
	SYS_SYNC_NOT_ADAPTIVE        = 0x2000,
	SYS_SYNC_ATTR_ADAPTIVE_MASK  = 0xf000,
};

enum ppu_thread_status : u32;

struct ppu_non_sleeping_count_t
{
	bool has_running; // no actual count for optimization sake
	u32 onproc_count;
};

// Base class for some kernel objects (shared set of 8192 objects).
struct lv2_obj
{
	static const u32 id_step = 0x100;
	static const u32 id_count = 8192;
	static constexpr std::pair<u32, u32> id_invl_range = {0, 8};

private:
	enum thread_cmd : s32
	{
		yield_cmd = smin,
		enqueue_cmd,
	};

	// Function executed under IDM mutex, error will make the object creation fail and the error will be returned
	CellError on_id_create()
	{
		exists++;
		return {};
	}

public:
	SAVESTATE_INIT_POS(4); // Dependency on PPUs

	lv2_obj() noexcept = default;
	lv2_obj(u32 i) noexcept : exists{ i } {}
	lv2_obj(lv2_obj&& rhs) noexcept : exists{ +rhs.exists } {}
	lv2_obj(utils::serial&) noexcept {}
	lv2_obj& operator=(lv2_obj&& rhs) noexcept { exists = +rhs.exists; return *this; }
	void save(utils::serial&) {}

	// Existence validation (workaround for shared-ptr ref-counting)
	atomic_t<u32> exists = 0;

	template <typename Ptr>
	static bool check(Ptr&& ptr)
	{
		return ptr && ptr->exists;
	}

	// wrapper for name64 string formatting
	struct name_64
	{
		u64 data;
	};

	static std::string name64(u64 name_u64);

	// Find and remove the object from the linked list
	template <bool ModifyNode = true, typename T>
	static T* unqueue(T*& first, T* object, T* T::* mem_ptr = &T::next_cpu)
	{
		auto it = +first;

		if (it == object)
		{
			atomic_storage<T*>::release(first, it->*mem_ptr);

			if constexpr (ModifyNode)
			{
				atomic_storage<T*>::release(it->*mem_ptr, nullptr);
			}

			return it;
		}

		for (; it;)
		{
			const auto next = it->*mem_ptr + 0;

			if (next == object)
			{
				atomic_storage<T*>::release(it->*mem_ptr, next->*mem_ptr);

				if constexpr (ModifyNode)
				{
					atomic_storage<T*>::release(next->*mem_ptr, nullptr);
				}

				return next;
			}

			it = next;
		}

		return {};
	}

	// Remove an object from the linked set according to the protocol
	template <typename E, typename T>
	static E* schedule(T& first, u32 protocol, bool modify_node = true)
	{
		auto it = static_cast<E*>(first);

		if (!it)
		{
			return it;
		}

		auto parent_found = &first;

		if (protocol == SYS_SYNC_FIFO)
		{
			while (true)
			{
				const auto next = +it->next_cpu;

				if (next)
				{
					parent_found = &it->next_cpu;
					it = next;
					continue;
				}

				if (cpu_flag::again - it->state)
				{
					atomic_storage<T>::release(*parent_found, nullptr);
				}

				return it;
			}
		}

		auto prio = it->prio.load();
		auto found = it;

		while (true)
		{
			auto& node = it->next_cpu;
			const auto next = static_cast<E*>(node);

			if (!next)
			{
				break;
			}

			const auto _prio = static_cast<E*>(next)->prio.load();

			// This condition tests for equality as well so the earliest element to be pushed is popped
			if (_prio.prio < prio.prio || (_prio.prio == prio.prio && _prio.order < prio.order))
			{
				found = next;
				parent_found = &node;
				prio = _prio;
			}

			it = next;
		}

		if (cpu_flag::again - found->state)
		{
			atomic_storage<T>::release(*parent_found, found->next_cpu);

			if (modify_node)
			{
				atomic_storage<T>::release(found->next_cpu, nullptr);
			}
		}

		return found;
	}

	template <typename T>
	static void emplace(T& first, T object)
	{
		atomic_storage<T>::release(object->next_cpu, first);
		atomic_storage<T>::release(first, object);

		object->prio.atomic_op([order = ++g_priority_order_tag](std::common_type_t<decltype(std::declval<T>()->prio.load())>& prio)
		{
			if constexpr (requires { +std::declval<decltype(prio)>().preserve_bit; } )
			{
				if (prio.preserve_bit)
				{
					// Restoring state on load
					prio.preserve_bit = 0;
					return;
				}
			}

			prio.order = order;
		});
	}

private:
	// Remove the current thread from the scheduling queue, register timeout
	static bool sleep_unlocked(cpu_thread&, u64 timeout, u64 current_time);

	// Schedule the thread
	static bool awake_unlocked(cpu_thread*, s32 prio = enqueue_cmd);

public:
	static constexpr u64 max_timeout = u64{umax} / 1000;

	static bool sleep(cpu_thread& cpu, const u64 timeout = 0);

	static bool awake(cpu_thread* thread, s32 prio = enqueue_cmd);

	// Returns true on successful context switch, false otherwise
	static bool yield(cpu_thread& thread);

	static void set_priority(cpu_thread& thread, s32 prio)
	{
		ensure(prio + 512u < 3712);
		awake(&thread, prio);
	}

	static inline void awake_all()
	{
		awake({});
		g_to_awake.clear();
	}

	static void make_scheduler_ready();

	static std::pair<ppu_thread_status, u32> ppu_state(ppu_thread* ppu, bool lock_idm = true, bool lock_lv2 = true);

	static inline void append(cpu_thread* const thread)
	{
		g_to_awake.emplace_back(thread);
	}

	// Serialization related
	static void set_future_sleep(cpu_thread* cpu);
	static bool is_scheduler_ready();

	// Must be called under IDM lock
	static ppu_non_sleeping_count_t count_non_sleeping_threads();

	static inline bool has_ppus_in_running_state() noexcept
	{
		return count_non_sleeping_threads().has_running != 0;
	}

	static void set_yield_frequency(u64 freq, u64 max_allowed_tsx);

	static void cleanup();

	template <typename T>
	static inline u64 get_key(const T& attr)
	{
		return (attr.pshared == SYS_SYNC_PROCESS_SHARED ? +attr.ipc_key : 0);
	}

	template <typename T, typename F, typename... Args>
	static error_code create(u32 pshared, u64 ipc_key, s32 flags, F&& make, bool key_not_zero = true, Args&&... args)
	{
		switch (pshared)
		{
		case SYS_SYNC_PROCESS_SHARED:
		{
			if (key_not_zero && ipc_key == 0)
			{
				return CELL_EINVAL;
			}

			switch (flags)
			{
			case SYS_SYNC_NEWLY_CREATED:
			case SYS_SYNC_NOT_CARE:
			case SYS_SYNC_NOT_CREATE:
			{
				break;
			}
			default: return CELL_EINVAL;
			}

			break;
		}
		case SYS_SYNC_NOT_PROCESS_SHARED:
		{
			break;
		}
		default: return CELL_EINVAL;
		}

		CellError error{};

		if (!idm::import<lv2_obj, T>([&]() -> shared_ptr<T>
		{
			shared_ptr<T> result = make();

			auto finalize_construct = [&]() -> shared_ptr<T>
			{
				if ((error = result->on_id_create(std::forward<Args>(args)...)))
				{
					result.reset();
				}

				return std::move(result);
			};

			if (pshared != SYS_SYNC_PROCESS_SHARED)
			{
				// Creation of unique (non-shared) object handle
				return finalize_construct();
			}

			auto& ipc_container = g_fxo->get<ipc_manager<T, u64>>();

			if (flags == SYS_SYNC_NOT_CREATE)
			{
				result = ipc_container.get(ipc_key);

				if (!result)
				{
					error = CELL_ESRCH;
					return result;
				}

				// Run on_id_create() on existing object
				return finalize_construct();
			}

			bool added = false;
			std::tie(added, result) = ipc_container.add(ipc_key, finalize_construct, flags != SYS_SYNC_NEWLY_CREATED);

			if (error)
			{
				return {};
			}

			if (!added)
			{
				if (flags == SYS_SYNC_NEWLY_CREATED)
				{
					// Object already exists but flags does not allow it
					error = CELL_EEXIST;

					// We specified we do not want to peek pointer's value, result must be empty
					AUDIT(!result);
					return result;
				}

				// Run on_id_create() on existing object
				return finalize_construct();
			}

			return result;
		}))
		{
			return error ? error : CELL_EAGAIN;
		}

		return CELL_OK;
	}

	template <typename T>
	static void on_id_destroy(T& obj, u64 ipc_key, u64 pshared = umax)
	{
		if (pshared == umax)
		{
			// Default is to check key
			pshared = ipc_key != 0;
		}

		if (obj.exists-- == 1u && pshared)
		{
			g_fxo->get<ipc_manager<T, u64>>().remove(ipc_key);
		}
	}

	template <typename T>
	static shared_ptr<T> load(u64 ipc_key, shared_ptr<T> make, u64 pshared = umax)
	{
		if (pshared == umax ? ipc_key != 0 : pshared != 0)
		{
			g_fxo->need<ipc_manager<T, u64>>();

			g_fxo->get<ipc_manager<T, u64>>().add(ipc_key, [&]()
			{
				return make;
			});
		}

		// Ensure no error
		ensure(!make->on_id_create());
		return make;
	}

	template <typename T, typename Storage = lv2_obj>
	static std::function<void(void*)> load_func(shared_ptr<T> make, u64 pshared = umax)
	{
		const u64 key = make->key;
		return [ptr = load<T>(key, make, pshared)](void* storage) { *static_cast<atomic_ptr<Storage>*>(storage) = ptr; };
	}

	static bool wait_timeout(u64 usec, ppu_thread* cpu = {}, bool scale = true, bool is_usleep = false);

	static void notify_all() noexcept;

	// Can be called before the actual sleep call in order to move it out of mutex scope
	static void prepare_for_sleep(cpu_thread& cpu);
	static ppu_thread* get_running_ppu(u32 index);
#ifdef RPCS3_PS4
	static std::string ps4_scheduler_snapshot();

	// Guest sys_fs_read and sys_fs_write calls of this process (sys_fs.cpp counts them on entry, the HLE cellFsRead and
	// cellFsWrite included), for the stall watchdog: a game busy with its own file I/O is not frozen. Lock-free, never
	// falls; umax stays reserved for "unknown".
	static inline atomic_t<u64> g_ps4_fs_io_calls{0};

	static u64 ps4_fs_io_calls() noexcept
	{
		return g_ps4_fs_io_calls.observe();
	}

	static void ps4_count_fs_io() noexcept
	{
		g_ps4_fs_io_calls.fetch_add(1);
	}

	// 0.29: Orbis timed waits are tick-quantized (~1 ms), so 'As Host' turns a guest usleep(30) poll into a ~1 ms nap.
	// Guest usleeps of 1..ps4_precise_sleep_max_us us are served like upstream 'Usleep Only' (sched_yield loop in
	// wait_timeout, no lv2 timeout registration). On by default; /data/rpcs3/no-precise-sleep.flag restores 0.28.
	// 0.34: also off while 'Modo frio' is on (cool-mode.flag) and for a title whose PC database entry asks 'As Host'
	// explicitly (System.cpp calls ps4_set_precise_sleep_title on every boot, before any guest thread runs).
	static constexpr u64 ps4_precise_sleep_max_us = 100;
	static bool ps4_precise_sleep(u64 usec) noexcept;
	static void ps4_set_precise_sleep_title(bool allowed) noexcept;

	// 0.37: takes the current PPU thread out of the run queue for a blocking host wait (lv2_host_lock/lv2_host_wait
	// below), as lv2_obj::sleep does: answers a pending preemption, gives the slot to the next thread, frees the vm
	// passive-lock slot and sets cpu_flag::wait. Unlike sleep(), it never touches a thread that is not queued (one
	// already asleep in lv2 must not be put back by a later check_state) and never sleeps a signaled thread.
	static utils::ps4::host_detach detach_for_host_wait(ppu_thread& ppu);

	// 0.37: for the STALL dump, the host locks a PPU thread holds while it is blocked or parked and the lock it waits
	// for: "holds [0x.. 0x..(s)] waits 0x.." ((s): shared hold), either part omitted when unknown; empty if neither.
	static std::string ps4_lock_state_text(const cpu_thread& cpu);

	// 0.37 lockdep-lite, cpu_thread::check_state: the current PPU thread is being parked while holding host locks
	static void ps4_note_parked_with_locks(ppu_thread& ppu) noexcept;
#endif

	struct notify_all_t
	{
		notify_all_t() noexcept
		{
			g_postpone_notify_barrier = true;
		}

		notify_all_t(const notify_all_t&) = delete;

		static void cleanup()
		{
			for (auto& cpu : g_to_notify)
			{
				if (!cpu)
				{
					return;
				}

				// While IDM mutex is still locked (this function assumes so) check if the notification is still needed
				// Pending flag is meant for forced notification (if the CPU really has pending work it can restore the flag in theory)
				// Disabled to allow reservation notifications from here
				if (false && cpu != &g_to_notify && static_cast<const decltype(cpu_thread::state)*>(cpu)->none_of(cpu_flag::signal + cpu_flag::pending))
				{
					// Omit it (this is a void pointer, it can hold anything)
					cpu = &g_to_notify;
				}
			}
		}

		static void enqueue_on_top(const void* waiter)
		{
			g_to_notify[0] = waiter;
			g_to_notify[1] = nullptr;
		}

		~notify_all_t() noexcept
		{
			lv2_obj::notify_all();
		}
	};

	// Scheduler mutex
	static shared_mutex g_mutex;

	// Proirity tags
	static atomic_t<u64> g_priority_order_tag;

private:
	// Pending list of threads to run
	static thread_local std::vector<class cpu_thread*> g_to_awake;

	// Scheduler queue for active PPU threads
	static class ppu_thread* g_ppu;

	// Waiting for the response from
	static u32 g_pending;

	// Pending list of threads to notify (cpu_thread::state ptr)
	static thread_local std::add_pointer_t<const void> g_to_notify[4];

	// If a notify_all_t object exists locally, postpone notifications to the destructor of it (not recursive, notifies on the first destructor for safety)
	static thread_local bool g_postpone_notify_barrier;

	static void schedule_all(u64 current_time = 0);
};

#ifdef RPCS3_PS4
// 0.29: one line of guest sleep counters for the frontend's 60 s report (defined in lv2.cpp): precise vs host-served
// guest usleeps, spin time, measured host oversleep, short lv2 timeouts left on host waits and progress_backoff
// tick sleeps. Values are for the interval since the previous call plus totals. Thread-safe; never throws.
// 0.34: also why precise sleep is off, the PPU threads with the most host usleeps above 100 us, the share of time with
// more runnable PPU threads than slots, and the busiest syscalls of the interval.
std::string ps4_lv2_sleep_stats() noexcept;

// 0.37: slow paths of the helpers below (lv2.cpp)
void lv2_host_lock_slow(std::unique_lock<shared_mutex>& lock);
void lv2_host_lock_shared_slow(std::shared_lock<shared_mutex>& lock);
void lv2_host_wait_impl(ppu_thread* ppu, void (*wait)(void*), void* arg);
#endif

// Host lock of an HLE module or lv2 syscall (cellAudio, cellPad, ...) taken by a guest thread. On PS4 a PPU thread
// that finds it busy waits outside the lv2 run queue (util/ps4_host_wait.hpp) instead of blocking while queued, and
// never re-enters the queue while owning it; host threads, and PPU threads already holding another host lock, take
// it plainly. The uncontended path is one try_lock. Other builds: a plain lock, as std::lock_guard / reader_lock.
#ifdef RPCS3_PS4
inline std::unique_lock<shared_mutex> lv2_host_lock(shared_mutex& mtx)
{
	std::unique_lock<shared_mutex> lock(mtx, std::try_to_lock);

	if (!lock.owns_lock()) [[unlikely]]
	{
		lv2_host_lock_slow(lock);
	}

	return lock;
}

inline std::shared_lock<shared_mutex> lv2_host_lock_shared(shared_mutex& mtx)
{
	std::shared_lock<shared_mutex> lock(mtx, std::try_to_lock);

	if (!lock.owns_lock()) [[unlikely]]
	{
		lv2_host_lock_shared_slow(lock);
	}

	return lock;
}

// Runs a blocking host wait of 'ppu' (the current thread, or null for a host thread) outside the run queue
template <typename F>
void lv2_host_wait(ppu_thread* ppu, F&& wait)
{
	lv2_host_wait_impl(ppu, [](void* arg) { (*static_cast<std::remove_reference_t<F>*>(arg))(); }, const_cast<void*>(static_cast<const void*>(std::addressof(wait))));
}
#else
inline std::unique_lock<shared_mutex> lv2_host_lock(shared_mutex& mtx)
{
	return std::unique_lock<shared_mutex>(mtx);
}

inline std::shared_lock<shared_mutex> lv2_host_lock_shared(shared_mutex& mtx)
{
	return std::shared_lock<shared_mutex>(mtx);
}

template <typename F>
void lv2_host_wait(ppu_thread*, F&& wait)
{
	wait();
}
#endif
