/* std::condition_variable, rebuilt against this platform's errno values.
 *
 * The toolchain's prebuilt libc++.a is wrong here. On hardware (mupen64plus-next loading a ROM):
 *
 *     [orbis] libc++abi: terminating with uncaught exception of type std::__1::system_error:
 *             condition_variable timed_wait failed: Operation timed out
 *
 * A timeout should make wait_for return cv_status::timeout. condition_variable.cpp.o compares
 * against Linux's ETIMEDOUT:
 *
 *     322: 83 7d 94 6e    cmpl $0x6e, -0x6c(%rbp)     ; 0x6e = 110
 *
 * while this FreeBSD-based pthread returns 60 (as the SDK's bits/errno.h says), so every timeout
 * throws. This affects everything that goes through __do_timed_wait (std::future::wait_for,
 * shared_timed_mutex, ...), e.g. an idle worker queue.
 *
 * Not fixed in pthread_cond_timedwait: code compiled against the SDK headers correctly expects
 * 60, so returning 110 would break every direct caller.
 *
 * An archive member is all-or-nothing, so the member's other strong symbols (notify_one,
 * notify_all, wait) are reproduced here from libc++ 11 (_LIBCPP_VERSION 11000, the SDK's
 * version); only the constant differs. notify_all_at_thread_exit is omitted (it needs libc++'s
 * private per-thread bookkeeping and no core references it; the link would name it if one did).
 *
 * The proper fix is a libc++ rebuilt against the target's headers, in the OpenOrbis SDK.
 *
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>

#include <condition_variable>
#include <limits>
#include <system_error>

_LIBCPP_BEGIN_NAMESPACE_STD

void condition_variable::notify_one() _NOEXCEPT
{
   __libcpp_condvar_signal(&__cv_);
}

void condition_variable::notify_all() _NOEXCEPT
{
   __libcpp_condvar_broadcast(&__cv_);
}

void condition_variable::wait(unique_lock<mutex>& lk) _NOEXCEPT
{
   if (!lk.owns_lock())
      __throw_system_error(EPERM, "condition_variable::wait: mutex not locked");

   int ec = __libcpp_condvar_wait(&__cv_, lk.mutex()->native_handle());
   if (ec)
      __throw_system_error(ec, "condition_variable wait failed");
}

void condition_variable::__do_timed_wait(unique_lock<mutex>& lk,
      chrono::time_point<chrono::system_clock, chrono::nanoseconds> tp) _NOEXCEPT
{
   using namespace chrono;

   if (!lk.owns_lock())
      __throw_system_error(EPERM, "condition_variable::timed wait: mutex not locked");

   nanoseconds d = tp.time_since_epoch();

   /* Upstream's clamp, kept verbatim: a deadline further out than this overflows the timespec
    * on some platforms, and 0x59682F000000E941 ns is roughly the year 2262. */
   if (d > nanoseconds(0x59682F000000E941))
      d = nanoseconds(0x59682F000000E941);

   timespec ts;
   seconds  s = duration_cast<seconds>(d);
   typedef decltype(ts.tv_sec) ts_sec;
   const ts_sec ts_sec_max = numeric_limits<ts_sec>::max();

   if (s.count() < ts_sec_max)
   {
      ts.tv_sec  = static_cast<ts_sec>(s.count());
      ts.tv_nsec = static_cast<decltype(ts.tv_nsec)>((d - s).count());
   }
   else
   {
      ts.tv_sec  = ts_sec_max;
      ts.tv_nsec = 999999999;
   }

   int ec = __libcpp_condvar_timedwait(&__cv_, lk.mutex()->native_handle(), &ts);

   /* The fix: ETIMEDOUT here is the platform's 60, since this TU uses the SDK's headers. */
   if (ec != 0 && ec != ETIMEDOUT)
      __throw_system_error(ec, "condition_variable timed_wait failed");
}

_LIBCPP_END_NAMESPACE_STD
