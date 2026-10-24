#pragma once

#include "util/types.hpp"
#include "util/atomic.hpp"
#include "util/shared_ptr.hpp"

#include <thread>
#include <string>

// Hardware core layout
enum class native_core_arrangement : u32
{
	undefined,
	generic,
	intel_ht,
	amd_ccx
};

enum class thread_class : u32
{
	general = 0,
	ppu = 1,
	spu = 2,
	rsx = 0x55,
};

enum class thread_state : u32
{
	created = 0,  // Initial state
	aborting = 1, // The thread has been joined in the destructor or explicitly aborted
	errored = 2, // Set after the emergency_exit call
	finished = 3,  // Final state, always set at the end of thread execution
	mask = 3,
	destroying_context = 7, // Special value assigned to destroy data explicitly before the destructor
};

template <class Context>
class named_thread;

class thread_base;

template <typename Ctx, typename... Args>
struct result_storage
{
	static constexpr bool empty = true;

	using type = void;
};

template <typename Ctx, typename... Args> requires (!std::is_void_v<std::invoke_result_t<Ctx, Args&&...>>)
struct result_storage<Ctx, Args...>
{
	using T = std::invoke_result_t<Ctx, Args&&...>;

	static_assert(std::is_default_constructible_v<T>);

	alignas(T) std::byte data[sizeof(T)];

	static constexpr bool empty = false;

	using type = T;

	T* _get()
	{
		return reinterpret_cast<T*>(&data);
	}

	const T* _get() const
	{
		return reinterpret_cast<const T*>(&data);
	}

	void init() noexcept
	{
		new (data) T();
	}

	void destroy() noexcept
	{
		_get()->~T();
	}
};

template <typename T>
concept NamedThreadName = requires (const T&)
{
	std::string(T::thread_name);
};

// Base class for task queue (linked list)
class thread_future
{
	friend class thread_base;

	shared_ptr<thread_future> next{};

	thread_future* prev{};

protected:
	atomic_t<void(*)(const thread_base*, thread_future*)> exec{};

	atomic_t<u32> done{0};

public:
	// Get reference to the atomic variable for inspection and waiting for
	const auto& get_wait() const
	{
		return done;
	}

	// Wait (preset)
	void wait() const
	{
		done.wait(0);
	}
};

// Thread base class
class thread_base
{
public:
	// Native thread entry point function type
#ifdef _WIN32
	using native_entry = uint(__stdcall*)(void* arg);
#else
	using native_entry = void*(*)(void* arg);
#endif

	const native_entry entry_point;

	// Set name for debugger
	static void set_name(std::string name);

private:
	// Thread handle (platform-specific)
	atomic_t<u64> m_thread{0};

	// Thread cycles
	atomic_t<u64> m_cycles{0};

	atomic_t<u32> m_dummy{0};

	// Thread state
	atomic_t<u32> m_sync{0};

	// Thread name
	atomic_ptr<std::string> m_tname;

	// Thread task queue (reversed linked list)
	atomic_ptr<thread_future> m_taskq{};

#ifdef RPCS3_PS4
	// 0.29: slot of the RPCS3-owned 2 MiB stack this thread runs on (Thread.cpp PS4 stack pool), umax when
	// libkernel allocated the stack. ~thread_base gives it back to the pool after a successful pthread_join.
	u32 m_ps4_stack_slot = umax;

	// 0.39: lazy service thread (named_thread of a Context with ps4_lazy): 1 = stand-by, not started yet; 2 = being
	// started; 0 = started (or not lazy).
	atomic_t<u32> m_ps4_lazy{0};
#endif

	// Start thread
	void start();

#ifdef RPCS3_PS4
	// 0.29: start() for stand-by objects, without the 0.28 retry and without ensure(). false = the console
	// refused the thread (even on an own stack): m_sync is 'finished' again and m_thread stays 0, so join(),
	// operator=(finished) and both destructors return at once. Only for objects no other thread can see yet.
	bool try_start_native() noexcept;
#endif

	// Called at the thread start
	void initialize(void (*error_cb)());

	// Called at the thread end, returns self handle
	u64 finalize(thread_state result) noexcept;

	// Cleanup after possibly deleting the thread instance
	static native_entry finalize(u64 _self) noexcept;

	// Make entry point
	static native_entry make_trampoline(u64(*entry)(thread_base* _base));

	friend class thread_ctrl;

	template <class Context>
	friend class named_thread;

protected:
	thread_base(native_entry, std::string name) noexcept;

	~thread_base() noexcept;

public:
	// Get CPU cycles since last time this function was called. First call returns 0.
	u64 get_cycles();

	// Wait for the thread (it does NOT change thread state, and can be called from multiple threads)
	bool join(bool dtor = false) const;

	// Notify the thread
	void notify();

	// Get thread id
	u64 get_native_id() const;

	// Add work to the queue
	void push(shared_ptr<thread_future>);

#ifdef RPCS3_PS4
	// 0.39: starts a lazy stand-by thread on its first use (push() does it for task-queue threads); true when it runs
	// or was never lazy. A refusal (or a stopping emulator) leaves it stand-by for the next use. Any thread; never
	// while holding a lock the started thread needs at once.
	bool ps4_start_lazy() noexcept;

	// 0.39: still a never-started lazy thread.
	bool ps4_lazy_standby() const noexcept
	{
		return m_ps4_lazy != 0;
	}
#endif

private:
	// Clear task queue (execute unless aborting)
	void exec();
};

// Collection of global function for current thread
class thread_ctrl final
{
	// Current thread
	static thread_local thread_base* g_tls_this_thread;

	// Error handling details
	static thread_local void(*g_tls_error_callback)();

	// Target cpu core layout
	static atomic_t<native_core_arrangement> g_native_core_layout;

	friend class thread_base;

	// Optimized get_name() for logging
	static std::string get_name_cached();

public:
	// Get current thread name
	static std::string get_name()
	{
		if (!g_tls_this_thread)
		{
			return "not named_thread";
		}

		return *g_tls_this_thread->m_tname.load();
	}

	// Get thread name
	template <typename T>
	static std::string get_name(const named_thread<T>& thread)
	{
		return *static_cast<const thread_base&>(thread).m_tname.load();
	}

	// Set current thread name (not recommended)
	static void set_name(std::string name);

	// Set thread name (not recommended)
	template <typename T>
	static void set_name(named_thread<T>& thread, std::string name)
	{
		static_cast<thread_base&>(thread).m_tname.store(make_single<std::string>(name));

		if (g_tls_this_thread == std::addressof(static_cast<thread_base&>(thread)))
		{
			g_tls_this_thread->set_name(std::move(name));
		}
	}

	template <typename T>
	static u64 get_cycles(named_thread<T>& thread)
	{
		return static_cast<thread_base&>(thread).get_cycles();
	}

	template <typename T>
	static void notify(named_thread<T>& thread)
	{
		static_cast<thread_base&>(thread).notify();
	}

	template <typename T>
	static u64 get_native_id(named_thread<T>& thread)
	{
		return static_cast<thread_base&>(thread).get_native_id();
	}

	// Read current state, possibly executing some tasks
	static thread_state state();

	// Wait once with timeout. Infinite value is -1.
	static void wait_for(u64 usec, bool alert = true);

	// Wait once with time point, add_time is added to the time point.
	static void wait_until(u64* wait_time, u64 add_time = 0, u64 min_wait = 0, bool update_to_current_time = true);

	// Waiting with accurate timeout
	static void wait_for_accurate(u64 usec);

	// Wait.
	static inline void wait()
	{
		wait_for(-1, true);
	}

	// Wait for both thread sync var and provided atomic var
	template <uint Max, typename Func>
	static inline void wait_on_custom(Func&& setter, u64 usec = -1)
	{
		auto _this = g_tls_this_thread;

		if (_this->m_sync.bit_test_reset(2) || _this->m_taskq)
		{
			return;
		}

		atomic_wait::list<Max + 2> list{};
		list.template set<Max>(_this->m_sync, 0);
		list.template set<Max + 1>(_this->m_taskq);
		setter(list);
		list.wait(atomic_wait_timeout{usec <= 0xffff'ffff'ffff'ffff / 1000 ? usec * 1000 : 0xffff'ffff'ffff'ffff});
	}

	template <typename T, typename U>
	static inline void wait_on(T& wait, U old, u64 usec = -1)
	{
		wait_on_custom<1>([&](atomic_wait::list<3>& list) { list.template set<0>(wait, old); }, usec);
	}

	template <typename T>
	static inline void wait_on(T& wait)
	{
		wait_on_custom<1>([&](atomic_wait::list<3>& list) { list.template set<0>(wait); });
	}

	// Exit.
	[[noreturn]] static void emergency_exit(std::string_view reason);

	// Exit the current named thread as errored without reporting a fatal error.
	[[noreturn]] static void silent_exit() noexcept;

	// Get current thread (may be nullptr)
	static thread_base* get_current()
	{
		return g_tls_this_thread;
	}

	// Detect layout
	static void detect_cpu_layout();

	// Returns a core affinity mask. Set whether to generate the high priority set or not
	static u64 get_affinity_mask(thread_class group);

	// Sets the native thread priority
	static void set_native_priority(int priority);

	// Sets the preferred affinity mask for this thread
	static void set_thread_affinity_mask(u64 mask);

	// Get process affinity mask
	static u64 get_process_affinity_mask();

	// Miscellaneous
	static u64 get_thread_affinity_mask();

	// Get current thread stack addr and size
	static std::pair<void*, usz> get_thread_stack();

	// Sets the native thread priority and returns it to zero at destructor
	struct scoped_priority
	{
		explicit scoped_priority(int prio) noexcept
		{
			set_native_priority(prio);
		}

		scoped_priority(const scoped_priority&) = delete;

		scoped_priority& operator=(const scoped_priority&) = delete;

		~scoped_priority() noexcept
		{
			set_native_priority(0);
		}
	};

	// Get thread ID (works for all threads)
	static u64 get_tid();

	// Check whether current thread is main thread (usually Qt GUI)
	static bool is_main();

private:
	// Miscellaneous
	static const u64 process_affinity_mask;
};

#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(no_unique_address)
#define NO_UNIQUE_ADDRESS [[no_unique_address]]
#else
#define NO_UNIQUE_ADDRESS
#endif
#else
#define NO_UNIQUE_ADDRESS
#endif

// Used internally
template <bool Discard, typename Ctx, typename... Args>
class thread_future_t : public thread_future, result_storage<Ctx, std::conditional_t<Discard, int, void>, Args...>
{
	NO_UNIQUE_ADDRESS decltype(std::make_tuple(std::forward<Args>(std::declval<Args>())...)) m_args;

	NO_UNIQUE_ADDRESS Ctx m_func;

	using future = thread_future_t;

public:
	thread_future_t(Ctx&& func, Args&&... args) noexcept
		: m_args(std::forward<Args>(args)...)
		, m_func(std::forward<Ctx>(func))
	{
		thread_future::exec.raw() = +[](const thread_base* tb, thread_future* tf)
		{
			const auto _this = static_cast<future*>(tf);

			if (!tb) [[unlikely]]
			{
				if constexpr (!future::empty && !Discard)
				{
					_this->init();
				}

				return;
			}

			if constexpr (future::empty || Discard)
			{
				std::apply(_this->m_func, std::move(_this->m_args));
			}
			else
			{
				new (_this->_get()) decltype(auto)(std::apply(_this->m_func, std::move(_this->m_args)));
			}
		};
	}

	~thread_future_t() noexcept
	{
		if constexpr (!future::empty && !Discard)
		{
			if (!this->exec)
			{
				this->destroy();
			}
		}
	}

	decltype(auto) get()
	{
		while (this->exec)
		{
			this->wait();
		}

		if constexpr (!future::empty && !Discard)
		{
			return *this->_get();
		}
	}

	decltype(auto) get() const
	{
		while (this->exec)
		{
			this->wait();
		}

		if constexpr (!future::empty && !Discard)
		{
			return *this->_get();
		}
	}
};

namespace stx
{
	struct launch_retainer;

	extern atomic_t<u32> g_launch_retainer;

#ifdef RPCS3_PS4
	// 0.29: tag of the PS4 fallible-start constructors. named_thread / named_thread_group built with it stay
	// stand-by until ps4_try_start(); a thread the console refuses is reported instead of ending the app.
	struct try_launch_t
	{
		explicit try_launch_t() = default;
	};

	inline constexpr try_launch_t try_launch{};
#endif
}

#ifdef RPCS3_PS4
// 0.39: a Context declaring `static constexpr bool ps4_lazy = true` (service threads most games never use) can be built
// stand-by and started on its first use (thread_base::ps4_start_lazy, also from push()), which keeps slots of the
// console's per-process thread cap for the game. 0.40: opt-in (row lazy_services of util/ps4_safe_mode.hpp,
// lazy-service-threads.flag); by default every such thread starts at boot, as in 0.38.
template <typename Context>
concept ps4_lazy_context = requires { requires Context::ps4_lazy; };

// 0.40: lazy service threads for this boot (row lazy_services; keep-service-threads.flag forces them off).
bool ps4_lazy_fxo_threads_enabled() noexcept;

// 0.39: default-named contexts whose body returns at once on the console (Camera and Microphone with the Null handler,
// CPU Profiler with every profiler off, PPU Syscall Usage, GemUpdateThread without a real Move handler, RsxAudio Backend
// without the rsxaudio provider; 0.40: NP Handler Thread while PSN is not RPCN): true = build them stand-by and never
// start them (0.38 started them, and each finished thread stayed unjoined until the stop, holding a slot of the
// console's thread cap). Decided in Thread.cpp from the name and the configuration (the same conditions as those
// bodies); logs one line per thread it keeps stand-by. False with keep-service-threads.flag (row idle_services_skip).
bool ps4_service_idle(std::string_view name) noexcept;

// 0.40: np_handler::operator() ("NP Handler Thread") returns at once unless the PSN status is RPCN. Its object is still
// built (the network modules use it); only the thread is kept stand-by. The reason ps4_service_idle() logs, or nullptr.
constexpr const char* ps4_np_handler_idle(bool psn_is_rpcn) noexcept
{
	return psn_is_rpcn ? nullptr : "PSN not RPCN";
}

// 0.39: wake counter of a lazy thread whose work is always announced by notify_one() on this counter (and which only
// works while woken through it): Owner::ps4_lazy_start() (static) starts the owner's named_thread first. Otherwise an
// atomic_t<u32>.
template <typename Owner>
struct ps4_lazy_wake_counter : atomic_t<u32>
{
	using atomic_t<u32>::atomic_t;
	using atomic_t<u32>::operator=;

	void notify_one()
	{
		Owner::ps4_lazy_start();
		atomic_t<u32>::notify_one();
	}
};
#endif

// Derived from the callable object Context, possibly a lambda
template <class Context>
class named_thread final : public Context, result_storage<Context>, thread_base
{
	using result = result_storage<Context>;
	using thread = thread_base;

	static u64 entry_point(thread_base* _base)
	{
		return static_cast<named_thread*>(_base)->entry_point2();
	}

	u64 entry_point2()
	{
		while (u32 value = stx::g_launch_retainer)
		{
			stx::g_launch_retainer.wait(value);
		}

		thread::initialize([]()
		{
			if constexpr (!result::empty)
			{
				// Construct using default constructor in the case of failure
				static_cast<result*>(static_cast<named_thread*>(thread_ctrl::get_current()))->init();
			}
		});

		if constexpr (result::empty)
		{
			// No result
			if constexpr (std::is_invocable_v<Context>)
			{
				Context::operator()();
			}
			else
			{
				// Default event loop
				while (thread_ctrl::state() != thread_state::aborting)
				{
					thread_ctrl::wait();
				}
			}
		}
		else
		{
			// Construct the result using placement new (copy elision should happen)
			new (result::_get()) decltype(auto)(Context::operator()());
		}

		return thread::finalize(thread_state::finished);
	}

#if defined(ARCH_X64)
	static inline thread::native_entry trampoline = thread::make_trampoline(entry_point);
#else
#ifdef _WIN32
	static uint trampoline(void* arg)
#else
	static void* trampoline(void* arg)
#endif
	{
		if (const auto next = thread_base::finalize(entry_point(static_cast<thread_base*>(arg))))
		{
			return next(thread_ctrl::get_current());
		}

		return {};
	}
#endif

	friend class thread_ctrl;

public:
	// Forwarding constructor with default name (also potentially the default constructor)
	template <typename... Args> requires (std::is_constructible_v<Context, Args&&...>) && (!(std::is_same_v<std::remove_cvref_t<Args>, stx::launch_retainer> || ...)) && (NamedThreadName<Context>)
	named_thread(Args&&... args) noexcept
		: Context(std::forward<Args>(args)...)
		, thread(trampoline, std::string(Context::thread_name))
	{
#ifdef RPCS3_PS4
		// The same choice, in the same order, as the launch_retainer constructor below
		if constexpr (std::is_constructible_v<std::string_view, decltype(Context::thread_name)>)
		{
			// Service threads have a fixed name; PPU/SPU contexts name each instance and are never idle services
			if (ps4_service_idle(std::string_view(Context::thread_name)))
			{
				// 0.39: stand-by for good (its body would return at once)
				m_sync |= static_cast<u32>(thread_state::finished);
				thread::m_ps4_lazy = 1;
				return;
			}
		}

		if constexpr (ps4_lazy_context<Context>)
		{
			if (ps4_lazy_fxo_threads_enabled())
			{
				// 0.39: stand-by (as the launch_retainer constructor) until its first use
				m_sync |= static_cast<u32>(thread_state::finished);
				thread::m_ps4_lazy = 1;
				return;
			}
		}
#endif
		thread::start();
	}

	// Forwarding constructor with default name, does not automatically run the thread
	template <typename... Args> requires (std::is_constructible_v<Context, Args&&...>) && (NamedThreadName<Context>)
	named_thread(const stx::launch_retainer&, Args&&... args) noexcept
		: Context(std::forward<Args>(args)...)
		, thread(trampoline, std::string(Context::thread_name))
	{
		// Create a stand-by thread context
		m_sync |= static_cast<u32>(thread_state::finished);
#ifdef RPCS3_PS4
		// g_fxo builds its service threads with this constructor and starts them with '= thread_state::created' (the
		// launch loop of fixed_typemap.hpp), so the stand-by choice of the default-name constructor is made here too.
		// Only for the plain build without arguments: PPU/SPU/HLE threads (built with their parameters) and objects
		// restored from a savestate are not affected, and PPU/SPU contexts name each instance (their thread_name is
		// not a fixed string).
		if constexpr (sizeof...(Args) == 0)
		{
			if constexpr (std::is_constructible_v<std::string_view, decltype(Context::thread_name)>)
			{
				if (ps4_service_idle(std::string_view(Context::thread_name)))
				{
					// Never started: its body would return at once in this configuration
					thread::m_ps4_lazy = 1;
				}
			}

			if constexpr (ps4_lazy_context<Context>)
			{
				if (thread::m_ps4_lazy.load() == 0 && ps4_lazy_fxo_threads_enabled())
				{
					// lazy-service-threads.flag (opt-in): started on its first use
					thread::m_ps4_lazy = 1;
				}
			}
		}
#endif
	}

	// Normal forwarding constructor
	template <typename... Args> requires (std::is_constructible_v<Context, Args&&...>) && (!NamedThreadName<Context>)
	named_thread(std::string name, Args&&... args) noexcept
		: Context(std::forward<Args>(args)...)
		, thread(trampoline, std::move(name))
	{
		thread::start();
	}

	// Lambda constructor, also the implicit deduction guide candidate
	named_thread(std::string_view name, Context&& f) noexcept requires (!NamedThreadName<Context>)
		: Context(std::forward<Context>(f))
		, thread(trampoline, std::string(name))
	{
		thread::start();
	}

#ifdef RPCS3_PS4
	// 0.29: stand-by context with an explicit name, for any Context (lambdas, unnamed contexts such as
	// spu_llvm_worker). Nothing runs until ps4_try_start(). Contexts with a thread_name (ppu_thread,
	// spu_thread, ...) can also use the launch_retainer constructor and then ps4_try_start().
	template <typename... Args> requires (std::is_constructible_v<Context, Args&&...>)
	named_thread(stx::try_launch_t, std::string name, Args&&... args) noexcept
		: Context(std::forward<Args>(args)...)
		, thread(trampoline, std::move(name))
	{
		m_sync |= static_cast<u32>(thread_state::finished);
	}

	// 0.29: run a stand-by context (try_launch or launch_retainer constructor). false = the console refused the
	// thread, also on an RPCS3-owned rescue stack: the object never runs and can be destroyed (or kept) at once.
	// Only for objects no other thread can see yet. Never sleeps, never retries, never ends the app.
	[[nodiscard]] bool ps4_try_start() noexcept
	{
		static_assert(result::empty, "ps4_try_start: contexts with a result are not supported");
		ensure(operator thread_state() == thread_state::finished);
		return thread::try_start_native();
	}

	// 0.39: start a lazy service thread on its first use (see ps4_lazy_context); true when it runs or was never lazy
	bool ps4_start_lazy() noexcept
	{
		return thread::ps4_start_lazy();
	}

	// 0.39: still a never-started lazy thread
	bool ps4_lazy_standby() const noexcept
	{
		return thread::ps4_lazy_standby();
	}
#endif

	named_thread(const named_thread&) = delete;

	named_thread& operator=(const named_thread&) = delete;

	// Wait for the completion and access result (if not void)
	[[nodiscard]] decltype(auto) operator()() noexcept
	{
		thread::join();

		if constexpr (!result::empty)
		{
			return *result::_get();
		}
	}

	// Wait for the completion and access result (if not void)
	[[nodiscard]] decltype(auto) operator()() const noexcept
	{
		thread::join();

		if constexpr (!result::empty)
		{
			return *result::_get();
		}
	}

	// Send command to the thread to invoke directly (references should be passed via std::ref())
	template <bool Discard = true, typename Arg, typename... Args>
	auto operator()(Arg&& arg, Args&&... args) noexcept
	{
		// Overloaded operator() of the Context.
		constexpr bool v1 = std::is_invocable_v<Context, Arg&&, Args&&...>;

		// Anything invocable, not necessarily involving the Context.
		constexpr bool v2 = std::is_invocable_v<Arg&&, Args&&...>;

		// Could be pointer to a non-static member function (or data member) of the Context.
		constexpr bool v3 = std::is_member_pointer_v<std::decay_t<Arg>> && std::is_invocable_v<Arg, Context&, Args&&...>;

		// Only one invocation type shall be valid, otherwise we don't know.
		static_assert((v1 + v2 + v3) == 1, "Ambiguous or invalid named_thread call.");

		if constexpr (v1)
		{
			using future = thread_future_t<Discard, Context&, Arg, Args...>;

			single_ptr<future> target = make_single<future>(*static_cast<Context*>(this), std::forward<Arg>(arg), std::forward<Args>(args)...);

			if constexpr (!Discard)
			{
				shared_ptr<future> result = std::move(target);

				// Copy result
				thread::push(result);
				return result;
			}
			else
			{
				// Move target
				thread::push(std::move(target));
				return;
			}
		}
		else if constexpr (v2)
		{
			using future = thread_future_t<Discard, Arg, Args...>;

			single_ptr<future> target = make_single<future>(std::forward<Arg>(arg), std::forward<Args>(args)...);

			if constexpr (!Discard)
			{
				shared_ptr<future> result = std::move(target);
				thread::push(result);
				return result;
			}
			else
			{
				thread::push(std::move(target));
				return;
			}
		}
		else if constexpr (v3)
		{
			using future = thread_future_t<Discard, Arg, Context&, Args...>;

			single_ptr<future> target = make_single<future>(std::forward<Arg>(arg), std::ref(*static_cast<Context*>(this)), std::forward<Args>(args)...);

			if constexpr (!Discard)
			{
				shared_ptr<future> result = std::move(target);
				thread::push(result);
				return result;
			}
			else
			{
				thread::push(std::move(target));
				return;
			}
		}
	}

	// Access thread state
	operator thread_state() const noexcept
	{
		return static_cast<thread_state>(thread::m_sync.load() & 3);
	}

	named_thread& operator=(thread_state s) noexcept
	{
		if (s == thread_state::created)
		{
#ifdef RPCS3_PS4
			// A stand-by service (idle, or lazy with lazy-service-threads.flag) stays stand-by: ps4_start_lazy() and
			// push() start it on its first use. One that such a use already started is running and needs nothing.
			if (thread::m_ps4_lazy.load() != 0 || thread::m_thread.load() != 0)
			{
				return *this;
			}
#endif
			// Run thread
			ensure(operator thread_state() == thread_state::finished);
			thread::start();
			return *this;
		}

		bool notify_sync = false;

		// Try to abort by assigning thread_state::aborting/finished
		// Join thread by thread_state::finished
		if (s >= thread_state::aborting && thread::m_sync.fetch_op([](u32& v) { return !(v & 3) && (v |= 1); }).second)
		{
			notify_sync = true;
		}

		if constexpr (std::is_assignable_v<Context&, thread_state>)
		{
			static_cast<Context&>(*this) = thread_state::aborting;
		}

		if (notify_sync)
		{
			// Notify after context abortion has been made so all conditions for wake-up be satisfied by the time of notification
			thread::m_sync.notify_all();
		}

		if (s == thread_state::finished || s == thread_state::destroying_context)
		{
			// This participates in emulation stopping, use destruction-alike semantics
			thread::join(true);
		}

		if (s == thread_state::destroying_context)
		{
			if constexpr (std::is_assignable_v<Context&, thread_state>)
			{
				static_cast<Context&>(*this) = thread_state::destroying_context;
			}
		}

		return *this;
	}

	// Context type doesn't need virtual destructor
	~named_thread() noexcept
	{
		// Assign aborting state forcefully and join thread
		operator=(thread_state::finished);

		if constexpr (!result::empty)
		{
			result::destroy();
		}
	}
};

#ifdef RPCS3_PS4
// 0.29: named_thread worker(stx::try_launch, "name", lambda) deduces the context like the lambda constructor
template <typename Name, typename Context>
named_thread(stx::try_launch_t, Name&&, Context&&) -> named_thread<std::remove_cvref_t<Context>>;
#endif

// Group of named threads, similar to named_thread
template <class Context>
class named_thread_group final
{
	using Thread = named_thread<Context>;

	u32 m_count = 0;

	Thread* m_threads;

	void init_threads()
	{
		m_threads = static_cast<Thread*>(::operator new(sizeof(Thread) * m_count, std::align_val_t{alignof(Thread)}));
	}

#ifdef RPCS3_PS4
	// 0.29: build slot m_count stand-by and start it. On refusal the slot (and its context, e.g. a held
	// semaphore permit) is destroyed at once and false is returned; m_count is not advanced, so join(),
	// operator[] and ~named_thread_group only ever see threads that run.
	template <typename... Args>
	bool ps4_try_emplace(std::string name, Args&&... args) noexcept
	{
		Thread* const slot = new (static_cast<void*>(m_threads + m_count)) Thread(stx::try_launch, std::move(name), std::forward<Args>(args)...);

		if (slot->ps4_try_start())
		{
			return true;
		}

		slot->~Thread();
		return false;
	}
#endif

public:
	// Lambda constructor, also the implicit deduction guide candidate
	named_thread_group(std::string_view name, u32 count, Context&& f) noexcept
		: m_count(count)
		, m_threads(nullptr)
	{
		if (count == 0)
		{
			return;
		}

		init_threads();

		// Create all threads
		for (u32 i = 0; i < m_count - 1; i++)
		{
			// Copy the context
			new (static_cast<void*>(m_threads + i)) Thread(std::string(name) + std::to_string(i + 1), static_cast<const Context&>(f));
		}

		// Move the context (if movable)
		new (static_cast<void*>(m_threads + m_count - 1)) Thread(std::string(name) + std::to_string(m_count), std::forward<Context>(f));
	}

	// Constructor with a function performed before adding more threads
	template <typename CheckAndPrepare>
	named_thread_group(std::string_view name, u32 count, Context&& f, CheckAndPrepare&& check) noexcept
		: m_count(count)
		, m_threads(nullptr)
	{
		if (count == 0)
		{
			return;
		}

		init_threads();
		m_count = 0;

		// Create all threads
		for (; m_count < count - 1; m_count++)
		{
			// Copy the context
			std::remove_cvref_t<Context> context(static_cast<const Context&>(f));

			// Perform the check and additional preparations for each context
			if (!std::invoke(std::forward<CheckAndPrepare>(check), m_count, context))
			{
				return;
			}

#ifdef RPCS3_PS4
			// 0.29: stop at the first thread the console refuses; that slot is destroyed at once (its context
			// releases what check() acquired, e.g. a jit_core_allocator permit) and the caller does the rest.
			if (!ps4_try_emplace(std::string(name) + std::to_string(m_count + 1), std::move(context)))
			{
				return;
			}
#else
			new (static_cast<void*>(m_threads + m_count)) Thread(std::string(name) + std::to_string(m_count + 1), std::move(context));
#endif
		}

		// Move the context (if movable)
		std::remove_cvref_t<Context> context(std::forward<Context>(f));

		if (!std::invoke(std::forward<CheckAndPrepare>(check), m_count, context))
		{
			return;
		}

#ifdef RPCS3_PS4
		if (!ps4_try_emplace(std::string(name) + std::to_string(m_count + 1), std::move(context)))
		{
			return;
		}
#else
		new (static_cast<void*>(m_threads + m_count)) Thread(std::string(name) + std::to_string(m_count + 1), std::move(context));
#endif
		m_count++;
	}

	// Default constructor
	named_thread_group(std::string_view name, u32 count) noexcept
		: m_count(count)
		, m_threads(nullptr)
	{
		if (count == 0)
		{
			return;
		}

		init_threads();

		// Create all threads
		for (u32 i = 0; i < m_count; i++)
		{
			new (static_cast<void*>(m_threads + i)) Thread(std::string(name) + std::to_string(i + 1));
		}
	}

#ifdef RPCS3_PS4
	// 0.29: default-context group that starts as many of `count` threads as the console grants, stopping at the
	// first refusal. size() is the number actually running, possibly 0 (the caller must then do the work itself).
	named_thread_group(stx::try_launch_t, std::string_view name, u32 count) noexcept
		: m_count(0)
		, m_threads(nullptr)
	{
		if (count == 0)
		{
			return;
		}

		m_threads = static_cast<Thread*>(::operator new(sizeof(Thread) * count, std::align_val_t{alignof(Thread)}));

		while (m_count < count && ps4_try_emplace(std::string(name) + std::to_string(m_count + 1)))
		{
			m_count++;
		}
	}
#endif

	named_thread_group(const named_thread_group&) = delete;

	named_thread_group& operator=(const named_thread_group&) = delete;

	// Wait for completion
	bool join() const
	{
		bool result = true;

		for (u32 i = 0; i < m_count; i++)
		{
			std::as_const(*std::launder(m_threads + i))();

			if (std::as_const(*std::launder(m_threads + i)) != thread_state::finished)
				result = false;
		}

		return result;
	}

	// Join and access specific thread
	auto operator[](u32 index) const
	{
		return std::as_const(*std::launder(m_threads + index))();
	}

	// Join and access specific thread
	auto operator[](u32 index)
	{
		return (*std::launder(m_threads + index))();
	}

	// Dumb iterator
	auto begin()
	{
		return std::launder(m_threads);
	}

	// Dumb iterator
	auto end()
	{
		return m_threads + m_count;
	}

	u32 size() const
	{
		return m_count;
	}

	~named_thread_group() noexcept
	{
		// Destroy all threads in reverse order (it should join them)
		for (u32 i = 0; i < m_count; i++)
		{
			std::launder(m_threads + (m_count - i - 1))->~Thread();
		}

		::operator delete(static_cast<void*>(m_threads), std::align_val_t{alignof(Thread)});
	}
};

usz map_workload(std::string_view thread_name, usz thread_count, usz count, std::function<void(usz)>&& func);
usz map_workload(std::string_view thread_name, usz thread_count, std::function<void()>&& func);

#ifdef RPCS3_PS4
// 0.29 PS4 thread runtime (Utilities/Thread.cpp). Each returns one line of text for startup.log / RPCS3.log.

// Boot: probes the private VA window of the RPCS3-owned stack pool and runs the own-stack self-test (crash
// marker /data/rpcs3/own-stacks-selftest.pending). Until it has run, no thread uses an own stack. Call once,
// from the main thread, after the RPCS3 log is up. Flags: /data/rpcs3/no-stack-rescue.flag (pool off),
// /data/rpcs3/own-stacks.flag (every thread_base thread starts on a pool stack, opt-in).
std::string ps4_thread_stacks_init() noexcept;

// Pool state: live/cached/committed stacks, rescues, deepest stack use (high-water of pool stacks only).
std::string ps4_thread_stacks_report() noexcept;

// Live RPCS3 threads grouped by name (PPU[..], SPU[..], 'RSX Worker #', ...) plus started/joined counts.
std::string ps4_thread_live_summary() noexcept;

// 0.29: registry of live RPCS3 threads, shared by ps4_thread_live_summary() and the sampling profiler.
// Every thread_base thread enters it on itself at the end of thread_base::initialize (after set_name, with its
// full name and stack bounds) and leaves it on itself at the top of thread_base::finalize(u64), which every
// exit path passes before pthread_exit: the normal return through the trampoline, thread_ctrl::emergency_exit
// and thread_ctrl::silent_exit (both call finalize(umax)). set_name renames the slot (full name; the kernel
// keeps 15 characters). Constant-initialized (usable before main), allocation-free and lock-free.
// The same exits also call orbis::threadNoteExit() right before pthread_exit, and ~thread_base calls
// orbis::threadNoteJoin() after each successful pthread_join (orbis-compat live / unjoined counts).
//
// Pin protocol (for code that signals or inspects other threads): pin(slot) succeeds only while the slot is
// live, and a pinned slot's thread cannot get past leave() (so it has not reached pthread_exit and its slot is
// not reused) until unpin(slot). leave() first makes self() return -1 (a late signal on that thread sees no
// slot), marks the slot leaving, then waits for the pins to drop (bounded at 1 s of process time; after that
// the slot is leaked, never reused, and the guarantee is void, rather than the thread hung). Hold pins for
// microseconds only: e.g. pin, read, pthread_kill(native, sig), unpin.
namespace ps4_thread_registry
{
	inline constexpr u32 slot_count = 512;
	inline constexpr u32 name_size = 40; // bytes, NUL included (longer names are truncated)

	enum : u32
	{
		slot_free = 0,
		slot_claim = 1, // owner is filling the slot
		slot_live = 2,
		slot_leave = 3, // owner is waiting for pins to drop (stays so forever if the slot leaked)
	};

	struct slot_info
	{
		u64 native;     // pthread_t of the thread (pthread_kill target)
		u64 stack_lo;   // lowest address of its stack, 0 = unknown (libkernel stack)
		u64 stack_hi;   // exclusive bound of a safe scan from its rsp: [rsp, stack_hi) is mapped when rsp < stack_hi
		                // (own stack: its top; libkernel stack: the top of orbis-compat's alt signal stack, which
		                // sits above every frame of the thread body; else a point just above the registering frame)
		u64 enter_us;   // sceKernelGetProcessTime() when it entered
		s32 tid;        // scePthreadGetthreadid() of the thread
		u32 gen;        // incremented every time the slot is claimed: tells a reused slot apart
		u32 stack_slot; // RPCS3-owned stack pool slot, umax = libkernel's stack
		char name[name_size];
	};

	// Called on the thread itself. thread_base does this; other threads (e.g. main) may register too.
	// Returns the slot, or -1 when the registry is full. A second call only renames.
	int enter(const char* name) noexcept;
	void rename(const char* name) noexcept; // current thread; no-op when not registered
	void leave() noexcept;                  // current thread; no-op when not registered

	// Slot of the calling thread or -1. Async-signal-safe (one static-TLS read).
	int self() noexcept;

	// Cheap unsynchronized views (relaxed loads) for prefiltering; async-signal-safe.
	u32 state(u32 slot) noexcept;
	u32 generation(u32 slot) noexcept;

	// Pin protocol (see above). Async-signal-safe.
	bool pin(u32 slot) noexcept;
	void unpin(u32 slot) noexcept;

	// Copies a slot. Only meaningful for a pinned slot or the caller's own slot. Async-signal-safe.
	bool read(u32 slot, slot_info& out) noexcept;

	// Optional callbacks for the profiler (one owner): on_enter runs on the thread right after its slot became
	// live, on_leave on the thread after the pins dropped and before the slot is freed (not for leaked slots).
	using hook = void (*)(u32 slot) noexcept;
	void set_hooks(hook on_enter, hook on_leave) noexcept;

	struct counters
	{
		u32 live;      // slots currently live
		u32 peak;      // highest live
		u64 entered;   // successful enter() calls
		u64 full;      // enter() calls that found no free slot
		u64 leaked;    // leave() calls that gave up waiting for pins
	};
	counters get_counters() noexcept;
}

// 0.39: what the console's per-process thread limit counts, as far as RPCS3 can see it: threads started through
// orbis-compat's pthread_create that have not exited (RPCS3, Mesa and audio threads; not Sony's own), plus thread_base
// threads that exited and were not joined yet. Relaxed atomic loads only (no lock, no allocation).
struct ps4_thread_load
{
	u32 live;     // orbis-compat live estimate
	u32 unjoined; // exited, not joined
	u32 total;    // live + unjoined
	u64 joined;   // successful joins noted so far
};

ps4_thread_load ps4_thread_load_now() noexcept;

// 0.39: true once a thread create was refused by the console in this process (test refusals excluded); 'out' = the
// load right after that first refusal.
bool ps4_thread_first_refusal(ps4_thread_load& out) noexcept;

// 0.40: thread census for startup.log: who holds the slots the console's cap counts: guest threads (PPU/SPU), the other
// threads of the emulator registry, threads outside the registry (Mesa, audio, libraries) and threads that exited and
// were not joined yet, with their names. Writes one NUL-terminated line into 'out' (truncated to fit) and returns its
// length (0 when size is 0). Allocation-free, lock-free (registry pins and relaxed loads) and formatted without stdio,
// so the fatal record of a crash signal can print it; any thread.
std::size_t ps4_thread_census_line(char* out, std::size_t size) noexcept;

// 0.40: true once after ps4_census_request() was called (the flag is cleared). The frontend's 1 s pass polls it and
// prints the census, so the creating or refusing thread never formats or writes the line itself.
bool ps4_census_due() noexcept;
void ps4_census_request() noexcept; // thread level changes, refused creates; any thread, signal-safe

// 0.40: a guest thread create that ends in CELL_EAGAIN because the console refused its host thread, or a refused
// must-succeed start ('what' names the kind, 'name' the thread): the running title's next boots use the lighter thread
// budget (adapt key threads=low) and its safe-mode level steps up (fail=threads, util/ps4_safe_mode.hpp). The notes are
// allocation-free and lock-free (adapt::note: once per key and session); one startup.log line per session through
// ps4_startup_checkpoint (not for signal handlers). The first call of a session writes the adapt file, so a create that
// returns to the game calls it without a lock that other creates need; a refused must-succeed start calls it just
// before its fatal.
void ps4_note_guest_thread_refused(const char* what, std::string_view name) noexcept;

// 0.39: first word of the system software version ("13.02"), "" when libkernel does not export the query. Read once.
const char* ps4_firmware_key() noexcept;

// 0.39: the guest access violation that paused (froze) the emulation, as "<thread>: reading|writing|executing 0x<addr>
// (unmapped|read-only memory)". True once per noted fault (the text is cleared); false when none is pending. Copies at
// most size-1 bytes, always NUL-terminated when size > 0. No allocation, no lock.
bool ps4_take_guest_fault(char* out, std::size_t size) noexcept;
#endif
