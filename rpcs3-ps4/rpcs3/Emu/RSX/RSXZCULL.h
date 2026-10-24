#pragma once

#include <util/types.hpp>
#include <util/vm.hpp>
#include <Emu/Memory/vm.h>

#include "Utilities/mutex.h"

#include "Utils/rsx_utils.h"

#include <vector>
#include <stack>
#include <string>
#include <unordered_map>

namespace rsx
{
	class thread;

	static inline std::string_view location_tostring(u32 location)
	{
		constexpr const char* location_names[2] = { "CELL_GCM_LOCATION_LOCAL", "CELL_GCM_LOCATION_MAIN" };
		return ::at32(location_names, location);
	}

	static inline u32 classify_location(u32 address)
	{
		return (address >= rsx::constants::local_mem_base) ? CELL_GCM_LOCATION_LOCAL : CELL_GCM_LOCATION_MAIN;
	}

	namespace reports
	{
		struct occlusion_query_info
		{
			u32 driver_handle;
			u32 result;
			u32 num_draws;
			u32 data_type;
			u64 sync_tag;
			u64 timestamp;
			bool pending;
			bool active;
			bool owned;
#ifdef RPCS3_PS4
			// A draw of this query was skipped because its program was not ready: the host count misses its samples.
			bool ps4_incomplete;
#endif
		};

		struct queued_report_write
		{
			u32 type = CELL_GCM_ZPASS_PIXEL_CNT;
			u32 counter_tag;
			u64 sync_tag;
			occlusion_query_info* query;
			queued_report_write* forwarder;

			vm::addr_t sink;                      // Memory location of the report
			std::vector<vm::addr_t> sink_alias;   // Aliased memory addresses
		};

		struct query_search_result
		{
			bool found;
			u32  raw_zpass_result;
			std::vector<occlusion_query_info*> queries;
		};

		struct query_stat_counter
		{
			u32 result;
			u32 flags;
		};

		struct sync_hint_payload_t
		{
			occlusion_query_info* query;
			vm::addr_t address;
			void* other_params;
		};

		struct MMIO_page_data_t : public rsx::ref_counted
		{
			utils::protection prot = utils::protection::rw;
		};

		enum sync_control
		{
			sync_none = 0,
			sync_defer_copy = 1, // If set, return a zcull intr code instead of forcefully reading zcull data
			sync_no_notify = 2   // If set, backend hint notifications will not be made
		};

		enum constants
		{
			max_zcull_delay_us    = 300,   // Delay before a report update operation is forced to retire
			min_zcull_tick_us     = 100,   // Default tick duration. To avoid hardware spam, we schedule peeks in multiples of this.
			occlusion_query_count = 2048,  // Number of occlusion query slots available. Real hardware actually has far fewer units before choking
			max_safe_queue_depth  = 1792,  // Number of in-flight queries before we start forcefully flushing data from the GPU device.
			max_stat_registers    = 8192   // Size of the statistics cache
		};

#ifdef RPCS3_PS4
		// Summary of the deferred report queue (ZCULL_control::ps4_state, RSX thread). Claimed writes have a memory
		// location and sit at the front of the queue; they reach memory in order.
		struct ps4_zcull_state
		{
			u32 pending = 0;              // queued report writes, claimed or not
			u32 claimed = 0;              // ... with a memory location
			u32 front_sink = 0;           // first queued write (0: none claimed)
			u32 front_type = 0;
			u32 front_draws = 0;          // draws of its occlusion query (0: no query or no draws)
			u64 front_sync_tag = 0;       // its query's sync tag
			u64 flushed_sync_tag = 0;     // newest query sync tag hinted to the backend (its command buffer submitted)
			u32 active_draws = 0;         // draws of the query being recorded (host queries active)
			s32 critical = 0;             // critical reports in flight (CPU reads a reports area)
			bool front_query = false;     // the front write waits for an occlusion query
			bool host_queries = false;    // occlusion queries are being recorded
			bool pages_accessed[2] = {};  // CPU read the reports area: local, main (reports are critical there)

			// The front query has draws newer than the last flush hint: its command buffer may be unsubmitted.
			bool front_unflushed() const { return front_query && front_draws && front_sync_tag > flushed_sync_tag; }

			std::string to_string() const;
		};
#endif

		class ZCULL_control
		{
		private:
			std::unordered_map<u32, MMIO_page_data_t> m_locked_pages[2];
			atomic_t<bool> m_pages_accessed[2] = { false, false };
			atomic_t<s32> m_critical_reports_in_flight = { 0 };
			shared_mutex m_pages_mutex;

			void on_report_enqueued(vm::addr_t address);
			void on_report_completed(vm::addr_t address);
			void disable_optimizations(class ::rsx::thread* ptimer, u32 location);

		protected:

			bool unit_enabled = false;           // The ZCULL unit is on
			bool surface_active = false;         // A surface in the ZCULL-monitored tile region has been loaded for rasterization
			bool stats_enabled = false;          // Collecting of ZCULL statistics is enabled (not same as pixels passing Z test!)
			bool zpass_count_enabled = false;    // Collecting of ZPASS statistics is enabled. If this is off, the counter does not increment
			bool host_queries_active = false;    // The backend/host is gathering Z data for the ZCULL unit

			std::array<occlusion_query_info, 2048> m_occlusion_query_data = {};
			std::stack<occlusion_query_info*> m_free_occlusion_pool{};

			occlusion_query_info* m_current_task = nullptr;
			u32 m_statistics_tag_id = 0;

			// Scheduling clock. Granunlarity is min_zcull_tick value.
			u64 m_tsc = 0;
			u64 m_next_tsc = 0;

			// Incremental tag used for tracking sync events. Hardware clock resolution is too low for the job.
			u64 m_sync_tag = 0;
			u64 m_timer = 0;

			std::vector<queued_report_write> m_pending_writes{};
			std::array<query_stat_counter, max_stat_registers> m_statistics_map{};

			// Enables/disables the ZCULL unit
			void set_active(class ::rsx::thread* ptimer, bool state, bool flush_queue);

			// Checks current state of the unit and applies changes
			void check_state(class ::rsx::thread* ptimer, bool flush_queue);

			// Sets up a new query slot and sets it to the current task
			void allocate_new_query(class ::rsx::thread* ptimer);

			// Free a query slot in use
			void free_query(occlusion_query_info* query);

			// Write report to memory
			void write(vm::addr_t sink, u64 timestamp, u32 type, u32 value);
			void write(queued_report_write* writer, u64 timestamp, u32 value);

			// Retire operation
			void retire(class ::rsx::thread* ptimer, queued_report_write* writer, u32 result);

			// Writes every claimed report, reading pending query results (blocking)
			void sync_claimed(class ::rsx::thread* ptimer);

		public:

			ZCULL_control();
			virtual ~ZCULL_control();

			ZCULL_control(const ZCULL_control&) = delete;
			ZCULL_control& operator=(const ZCULL_control&) = delete;

			void set_enabled(class ::rsx::thread* ptimer, bool state, bool flush_queue = false);
			void set_status(class ::rsx::thread* ptimer, bool surface_active, bool zpass_active, bool zcull_stats_active, bool flush_queue = false);

			// Read current zcull statistics into the address provided
			void read_report(class ::rsx::thread* ptimer, vm::addr_t sink, u32 type);

			// Clears current stat block and increments stat_tag_id
			void clear(class ::rsx::thread* ptimer, u32 type);

			// Forcefully flushes all
			void sync(class ::rsx::thread* ptimer);

			// Conditionally sync any pending writes if range overlaps
			flags32_t read_barrier(class ::rsx::thread* ptimer, u32 memory_address, u32 memory_range, flags32_t flags);
			flags32_t read_barrier(class ::rsx::thread* ptimer, u32 memory_address, occlusion_query_info* query);

			// Call once every 'tick' to update, optional address provided to partially sync until address is processed
			void update(class ::rsx::thread* ptimer, u32 sync_address = 0, bool hint = false);

			// Draw call notification
			void on_draw();

			// Sync hint notification
			void on_sync_hint(sync_hint_payload_t payload);

			// Check for pending writes
			bool has_pending() const { return !m_pending_writes.empty(); }

			// Search for query synchronized at address
			query_search_result find_query(vm::addr_t sink_address, bool all);

			// Copies queries in range rebased from source range to destination range
			u32 copy_reports_to(u32 start, u32 range, u32 dest);

			// Check paging issues
			bool on_access_violation(u32 address);

			// Optimization check
			bool is_query_result_urgent(u32 address) const { return m_pages_accessed[rsx::classify_location(address)]; }

#ifdef RPCS3_PS4
			// Queue summary (RSX thread only: reads the queue without a lock).
			ps4_zcull_state ps4_state() const;

			// ps4_state() as one line: pending and claimed counts, front {sink, type, draws, sync tag vs flushed},
			// pages accessed, critical reports in flight. RSX thread only.
			std::string ps4_describe() const;

			// True when the page holding `address` is still protected by this unit for deferred reports. Takes the
			// pages lock (shared): callable from an access-violation handler on any thread.
			bool ps4_page_locked(u32 address);

			// Submits the command buffer holding the query of the newest claimed report when its draws are newer
			// than the last flush hint (non-blocking). Returns true when a hint was sent. RSX thread only.
			bool ps4_hint(class ::rsx::thread* ptimer);

			// Writes every claimed report now (blocking query reads, bounded by the backend's query deadline).
			// Returns the number of queue entries retired. RSX thread only.
			u32 ps4_drain(class ::rsx::thread* ptimer);

			// The backend skipped the current draw because its program was not ready (call before the draw's
			// rsx::thread::end). With zcull_skip_visible the active query is marked incomplete, so a read-back that
			// finds no samples reports visible instead of occluded. RSX thread only.
			void ps4_on_skipped_draw();

			// zcull_skip_visible as read when this unit was created (one per renderer).
			bool ps4_skip_visible() const { return m_ps4_skip_visible; }

		private:
			bool m_ps4_skip_visible = true;

		public:
#endif

			// Backend methods (optional, will return everything as always visible by default)
			virtual void begin_occlusion_query(occlusion_query_info* /*query*/) {}
			virtual void end_occlusion_query(occlusion_query_info* /*query*/) {}
			virtual bool check_occlusion_query_status(occlusion_query_info* /*query*/) { return true; }
			virtual void get_occlusion_query_result(occlusion_query_info* query) { query->result = -1; }
			virtual void discard_occlusion_query(occlusion_query_info* /*query*/) {}
		};

		// Helper class for conditional rendering
		struct conditional_render_eval
		{
			bool enabled = false;
			bool eval_failed = false;
			bool hw_cond_active = false;
			bool reserved = false;

			std::vector<occlusion_query_info*> eval_sources;
			u64 eval_sync_tag = 0;
			u32 eval_address = 0;

			// Resets common data
			void reset();

			// Returns true if rendering is disabled as per conditional render test
			bool disable_rendering() const;

			// Returns true if a conditional render is active but not yet evaluated
			bool eval_pending() const;

			// Enable conditional rendering
			void enable_conditional_render(thread* pthr, u32 address);

			// Disable conditional rendering
			void disable_conditional_render(thread* pthr);

			// Sets data sources for predicate evaluation
			void set_eval_sources(std::vector<occlusion_query_info*>& sources);

			// Sets evaluation result. Result is true if conditional evaluation failed
			void set_eval_result(thread* pthr, bool failed);

			// Evaluates the condition by accessing memory directly
			void eval_result(thread* pthr);
		};
	}
}
