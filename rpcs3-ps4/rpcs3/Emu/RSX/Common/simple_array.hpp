#pragma once

#include <util/types.hpp>
#include <functional>
#include <algorithm>
#include <new>

#include "aligned_malloc.hpp"
#include "reverse_ptr.hpp"

#ifdef RPCS3_PS4
#include <atomic>
#include <cstdio>

#include "util/ps4_i18n.hpp"
#include "util/ps4_vram_policy.hpp"

// Allocation-free startup.log line and toast (ps4_fatal.cpp); the frontend's regular stop to the XMB (ps4_main.cpp).
void ps4_fatal_note(const char* prefix, const char* detail) noexcept;
void ps4_notify(const char* message) noexcept;
void ps4_request_return_to_menu();
#endif

namespace rsx
{
#ifdef RPCS3_PS4
	namespace ps4_simple_array_detail
	{
		inline std::atomic<u32> g_allocation_failures{ 0 };

		// The first failed allocation of the process gets one startup.log line. The heap is at its cap here, so
		// the line is formatted on the stack.
		inline void note_allocation_failure(u64 bytes, bool recovered) noexcept
		{
			if (g_allocation_failures.fetch_add(1, std::memory_order_relaxed) != 0)
			{
				return;
			}

			char detail[160];
			std::snprintf(detail, sizeof(detail), "allocation of %llu KiB failed; retried after the std::new_handler: %s",
				static_cast<unsigned long long>(bytes >> 10), recovered ? "ok" : "failed again");
			ps4_fatal_note("simple_array: ", detail);
		}

		// 0.38: the retry failed too. The first such failure of a game asks the frontend for its regular stop to the
		// XMB, with the cause named in startup.log and a toast, before the thread error that follows (the caller cannot
		// continue without the storage).
		inline void request_graceful_stop(u64 bytes) noexcept
		{
			char detail[128];
			std::snprintf(detail, sizeof(detail), "RSX container of %llu KiB not allocated after the std::new_handler retry",
				static_cast<unsigned long long>(bytes >> 10));

			if (!utils::ps4::vram::request_stop(utils::ps4::vram::stop_cause::cpu_heap, detail))
			{
				return;
			}

			ps4_fatal_note("simple_array: graceful stop requested (game stopped, back to the XMB): ", detail);
			ps4_notify(utils::ps4::i18n::tr(utils::ps4::i18n::str::TOAST_RPCS3_MEMORIA_EMULADOR_ESGOTADA_JOGO));
			ps4_request_return_to_menu();
		}
	}
#endif

	template <typename C, typename T>
	concept span_like =
		requires(C& c) {
			{ c.data() } -> std::convertible_to<const T*>;
			{ c.size() } -> std::integral;
	};

	template <typename T, typename U>
	concept is_trivially_comparable_v =
		requires (T t1, U t2) {
			{ t1 == t2 } -> std::same_as<bool>;
	};

	template <typename Ty, size_t Align=alignof(Ty)>
		requires std::is_trivially_destructible_v<Ty> && std::is_trivially_copyable_v<Ty>
	struct simple_array
	{
	public:
		using iterator = Ty*;
		using const_iterator = const Ty*;
		using reverse_iterator = reverse_pointer<Ty>;
		using const_reverse_iterator = reverse_pointer<const Ty>;
		using value_type = Ty;

	private:
		static constexpr u32 _local_capacity = std::max<u32>(64u / sizeof(Ty), 1u);
		alignas(Align) char _local_storage[_local_capacity * sizeof(Ty)];

		u32 _capacity = _local_capacity;
		Ty* _data = _local_capacity ? reinterpret_cast<Ty*>(_local_storage) : nullptr;
		u32 _size = 0;

		inline u32 offset(const_iterator pos)
		{
			return (_data) ? u32(pos - _data) : 0u;
		}

		bool is_local_storage() const
		{
			return _data == reinterpret_cast<const Ty*>(_local_storage);
		}

	public:
		simple_array() = default;

		simple_array(u32 initial_size)
		{
			reserve(initial_size);
			_size = initial_size;
		}

		simple_array(u32 initial_size, const Ty val)
		{
			reserve(initial_size);
			_size = initial_size;

			for (u32 n = 0; n < initial_size; ++n)
			{
				_data[n] = val;
			}
		}

		simple_array(const std::initializer_list<Ty>& args)
		{
			reserve(::size32(args));

			for (const auto& arg : args)
			{
				push_back(arg);
			}
		}

		simple_array(const simple_array& other)
		{
			resize(other._size);

			if (_size)
			{
				std::memcpy(_data, other._data, size_bytes());
			}
		}

		simple_array(simple_array&& other) noexcept
		{
			swap(other);
		}

		template <typename Container>
			requires span_like<Container, Ty>
		simple_array(const Container& container)
		{
			resize(container.size());

			if (_size)
			{
				std::memcpy(_data, container.data(), size_bytes());
			}
		}

		simple_array& operator=(const simple_array& other)
		{
			if (&other != this)
			{
				resize(other._size);

				if (_size)
				{
					std::memcpy(_data, other._data, size_bytes());
				}
			}

			return *this;
		}

		simple_array& operator=(simple_array&& other) noexcept
		{
			swap(other);
			return *this;
		}

		~simple_array()
		{
			if (_data)
			{
				if (!is_local_storage())
				{
					aligned_allocator::free(_data);
				}

				_data = nullptr;
				_size = _capacity = 0;
			}
		}

		void swap(simple_array<Ty>& that) noexcept
		{
			if (!_size && !that._size)
			{
				// NOP. Surprisingly common
				return;
			}

			const auto _this_is_local = is_local_storage();
			const auto _that_is_local = that.is_local_storage();

			if (!_this_is_local && !_that_is_local)
			{
				std::swap(_capacity, that._capacity);
				std::swap(_size, that._size);
				std::swap(_data, that._data);
				return;
			}

			if (!_size)
			{
				*this = that;
				that.clear();
				return;
			}

			if (!that._size)
			{
				that = *this;
				clear();
				return;
			}

			if (_this_is_local != _that_is_local)
			{
				// Mismatched usage of the stack storage.
				rsx::simple_array<Ty> tmp{ *this };
				*this = that;
				that = tmp;
				return;
			}

			// Use memcpy to allow compiler optimizations
			Ty _stack_alloc[_local_capacity];
			std::memcpy(_stack_alloc, that._data, that.size_bytes());
			std::memcpy(that._data, _data, size_bytes());
			std::memcpy(_data, _stack_alloc, that.size_bytes());
			std::swap(_size, that._size);
		}

		void reserve(u32 size)
		{
			if (_capacity >= size)
			{
				return;
			}

#ifdef RPCS3_PS4
			// The CPU heap is capped: a failed allocation gets one retry after the std::new_handler (the memory
			// governor's emergency reserve, when armed) released memory. A second failure requests the graceful stop
			// of the game, then still ends in ensure().
			const auto allocate = [this, size]() -> Ty*
			{
				if (is_local_storage())
				{
					return static_cast<Ty*>(aligned_allocator::malloc<Align>(sizeof(Ty) * size));
				}

				// A failed realloc leaves the old block in place (aligned_malloc.hpp).
				return static_cast<Ty*>(aligned_allocator::realloc<Align>(_data, size_bytes(), sizeof(Ty) * size));
			};

			Ty* ptr = allocate();
			if (!ptr) [[unlikely]]
			{
				const u64 bytes = u64{ sizeof(Ty) } * size;
				if (const std::new_handler handler = std::get_new_handler())
				{
					handler();
				}

				ptr = allocate();
				ps4_simple_array_detail::note_allocation_failure(bytes, ptr != nullptr);

				if (!ptr)
				{
					ps4_simple_array_detail::request_graceful_stop(bytes);
				}
			}

			if (is_local_storage())
			{
				// Switch to heap storage
				ensure(ptr);
				std::memcpy(static_cast<void*>(ptr), _local_storage, size_bytes());
			}
			else
			{
				// Extend heap storage
				ensure(ptr); // "realloc() failed!"
			}

			_data = ptr;
#else
			if (is_local_storage())
			{
				// Switch to heap storage
				ensure(_data = static_cast<Ty*>(aligned_allocator::malloc<Align>(sizeof(Ty) * size)));
				std::memcpy(static_cast<void*>(_data), _local_storage, size_bytes());
			}
			else
			{
				// Extend heap storage
				ensure(_data = static_cast<Ty*>(aligned_allocator::realloc<Align>(_data, size_bytes(), sizeof(Ty) * size))); // "realloc() failed!"
			}
#endif

			_capacity = size;
		}

		template <typename T> requires UnsignedInt<T>
		void resize(T size)
		{
			const auto new_size = static_cast<u32>(size);
			reserve(new_size);
			_size = new_size;
		}

		void push_back(const Ty& val)
		{
			if (_size >= _capacity)
			{
				reserve(_capacity + 16);
			}

			_data[_size++] = val;
		}

		void push_back(Ty&& val)
		{
			if (_size >= _capacity)
			{
				reserve(_capacity + 16);
			}

			_data[_size++] = val;
		}

		template <typename... Args>
		void emplace_back(Args&&... args)
		{
			if (_size >= _capacity)
			{
				reserve(_capacity + 16);
			}

#ifdef RPCS3_PS4
			// OpenOrbis libc++ 11 lacks std::construct_at; this method is not
			// constexpr, so placement new has the same construction behavior.
			::new (static_cast<void*>(&_data[_size++])) Ty(std::forward<Args>(args)...);
#else
			std::construct_at(&_data[_size++], std::forward<Args&&>(args)...);
#endif
		}

		Ty pop_back()
		{
			return _data[--_size];
		}

		iterator insert(iterator pos, const Ty& val)
		{
			ensure(pos >= _data);
			const auto _loc = offset(pos);

			if (_size >= _capacity)
			{
				reserve(_capacity + 16);
				pos = _data + _loc;
			}

			if (_loc >= _size)
			{
				_data[_size++] = val;
				return pos;
			}

			AUDIT(_loc < _size);

			const auto remaining = (_size - _loc);
			std::memmove(pos + 1, pos, remaining * sizeof(Ty));

			*pos = val;
			_size++;

			return pos;
		}

		iterator insert(iterator pos, Ty&& val)
		{
			ensure(pos >= _data);
			const auto _loc = offset(pos);

			if (_size >= _capacity)
			{
				reserve(_capacity + 16);
				pos = _data + _loc;
			}

			if (_loc >= _size)
			{
				_data[_size++] = val;
				return pos;
			}

			AUDIT(_loc < _size);

			const u32 remaining = (_size - _loc);
			std::memmove(pos + 1, pos, remaining * sizeof(Ty));

			*pos = val;
			_size++;

			return pos;
		}

		iterator insert(iterator where, span_like<Ty> auto const& values)
		{
			ensure(where >= _data);
			const auto _loc = offset(where);
			const auto in_size = static_cast<u32>(values.size());
			const auto in_size_bytes = in_size * sizeof(Ty);

			reserve(_size + in_size);

			if (_loc >= _size)
			{
				where = _data + _size;
				std::memcpy(where, values.data(), in_size_bytes);
				_size += in_size;
				return where;
			}

			const u32 remaining_bytes = (_size - _loc) * sizeof(Ty);
			where = _data + _loc;
			std::memmove(where + in_size, where, remaining_bytes);
			std::memmove(where, values.data(), in_size_bytes);
			_size += in_size;
			return where;
		}

		void operator += (const rsx::simple_array<Ty>& that)
		{
			const auto old_size = _size;
			resize(_size + that._size);
			std::memcpy(data() + old_size, that.data(), that.size_bytes());
		}

		void clear()
		{
			_size = 0;
		}

		bool empty() const
		{
			return _size == 0;
		}

		u32 size() const
		{
			return _size;
		}

		u64 size_bytes() const
		{
			return _size * sizeof(Ty);
		}

		u32 size_bytes32() const
		{
			return _size * sizeof(Ty);
		}

		u32 capacity() const
		{
			return _capacity;
		}

		Ty& operator[] (u32 index)
		{
			return _data[index];
		}

		const Ty& operator[] (u32 index) const
		{
			return _data[index];
		}

		Ty* data()
		{
			return _data;
		}

		const Ty* data() const
		{
			return _data;
		}

		Ty& back()
		{
			return _data[_size - 1];
		}

		const Ty& back() const
		{
			return _data[_size - 1];
		}

		Ty& front()
		{
			return _data[0];
		}

		const Ty& front() const
		{
			return _data[0];
		}

		iterator begin()
		{
			return _data;
		}

		iterator end()
		{
			return _data ? _data + _size : nullptr;
		}

		const_iterator begin() const
		{
			return _data;
		}

		const_iterator end() const
		{
			return _data ? _data + _size : nullptr;
		}

		const_iterator cbegin() const
		{
			return _data;
		}

		const_iterator cend() const
		{
			return _data ? _data + _size : nullptr;
		}

		reverse_iterator rbegin()
		{
			return reverse_iterator(end() - 1);
		}

		reverse_iterator rend()
		{
			return reverse_iterator(begin() - 1);
		}

		const_reverse_iterator rbegin() const
		{
			return crbegin();
		}

		const_reverse_iterator rend() const
		{
			return crend();
		}

		const_reverse_iterator crbegin() const
		{
			return const_reverse_iterator(cend() - 1);
		}

		const_reverse_iterator crend() const
		{
			return const_reverse_iterator(cbegin() - 1);
		}

		bool any(std::predicate<const Ty&> auto predicate) const
		{
			for (auto it = begin(); it != end(); ++it)
			{
				if (std::invoke(predicate, *it))
				{
					return true;
				}
			}
			return false;
		}

		/**
		 * Note that find and find_if return pointers to objects and not iterators for simplified usage.
		 * It is functionally equivalent to retrieve a nullptr meaning empty object stored and nullptr meaning not found for all practical uses of this container.
		 */
		template <typename T = Ty>
			requires is_trivially_comparable_v<Ty, T>
		Ty* find(const T& value)
		{
			for (auto it = begin(); it != end(); ++it)
			{
				if (*it == value)
				{
					return &(*it);
				}
			}
			return nullptr;
		}

		// Remove when we switch to C++23
		template <typename T = Ty>
			requires is_trivially_comparable_v<Ty, T>
		const Ty* find(const T& value) const
		{
			return const_cast<simple_array<Ty, Align>*>(this)->find(value);
		}

		Ty* find_if(std::predicate<const Ty&> auto predicate)
		{
			for (auto it = begin(); it != end(); ++it)
			{
				if (std::invoke(predicate, *it))
				{
					return &(*it);
				}
			}
			return nullptr;
		}

		// Remove with C++23
		const Ty* find_if(std::predicate<const Ty&> auto predicate) const
		{
			return const_cast<simple_array<Ty, Align>*>(this)->find_if(predicate);
		}

		bool erase_if(std::predicate<const Ty&> auto predicate)
		{
			if (!_size)
			{
				return false;
			}

			bool ret = false;
			for (auto ptr = _data, last = _data + _size - 1; ptr <= last;)
			{
				if (predicate(*ptr))
				{
					ret = true;

					if (ptr == last)
					{
						// Popping the last entry from list. Just set the new size and exit
						_size--;
						break;
					}

					// Move item to the end of the list and shrink by 1
					std::memcpy(ptr, last, sizeof(Ty));
					_size--;
					last--;

					// Retest the same ptr which now has the previous tail item
					continue;
				}

				ptr++;
			}

			return ret;
		}

		simple_array<Ty> filter(std::predicate<const Ty&> auto predicate) const
		{
			simple_array<Ty> result;
			result.reserve(size());

			for (auto it = begin(); it != end(); ++it)
			{
				if (predicate(*it))
				{
					result.push_back(*it);
				}
			}
			return result;
		}

		simple_array<Ty>& sort(std::predicate<const Ty&, const Ty&> auto predicate)
		{
			if (_size < 2)
			{
				return *this;
			}

			std::sort(begin(), end(), predicate);
			return *this;
		}

		template <typename F, typename U = std::invoke_result_t<F, const Ty&>>
			requires (std::is_invocable_v<F, const Ty&> && std::is_trivially_destructible_v<U>)
		simple_array<U> map(F&& xform) const
		{
			simple_array<U> result;
			result.reserve(size());

			for (auto it = begin(); it != end(); ++it)
			{
				result.push_back(xform(*it));
			}
			return result;
		}

		template <typename F, typename U = std::invoke_result_t<F, const Ty&>>
			requires (std::is_invocable_v<F, const Ty&> && !std::is_trivially_destructible_v<U>)
		std::vector<U> map(F&& xform) const
		{
			std::vector<U> result;
			result.reserve(size());

			for (auto it = begin(); it != end(); ++it)
			{
				result.push_back(xform(*it));
			}
			return result;
		}

		template <typename F, typename U>
			requires std::is_invocable_r_v<U, F, const U&, const Ty&>
		U reduce(U initial_value, F&& reducer) const
		{
			U accumulate = initial_value;
			for (auto it = begin(); it != end(); ++it)
			{
				accumulate = reducer(accumulate, *it);
			}
			return accumulate;
		}

		template <typename F>
		void for_each(F&& func)
		{
			std::for_each(begin(), end(), func);
		}
	};
}
