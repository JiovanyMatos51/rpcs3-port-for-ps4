// Copyright © 2026 Mikołaj Mikołajczyk
// SPDX-License-Identifier: MIT
//
// UDP log transport.
//
// The implementation is in optional/, not in liborbis-compat.a, because it calls sceNet and would
// force -lSceNet on every consumer. Add optional/orbis_netlog.cpp to your target to use it.
//
// Fire-and-forget: sceNetSendto cannot stall the title, and a lost datagram is gone. For lines
// written by a dying process use the fatal channel in orbis_log.h instead.
//
// Register a va_list wrapper of orbis_netlog through orbis_set_log() to route this library's
// messages over the network. Host receivers are in scripts/ps4/ (log-receiver.py on 18194, logs.sh,
// peerfilter.py). Start the receiver before the title: boot lines are sent only once.
//
// Adapted from ps4doom's platform/ps4_netlog.c (MIT, same maintainer).
//
// Not written to stdout: on a real console stdout goes to the kernel debug buffer, and a write
// blocks when GoldHEN's klog server is not being drained (seconds per line inside a render loop).
//
// Target host/port are compile-time overridable and default to the direct cable link:
//   -DNETLOG_HOST=\"192.168.100.1\" -DNETLOG_PORT=18194
//
// Link with -lSceNet (the ps4-netlog CMake target does this for you).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Open the socket. Safe to call once, early in main(). On failure orbis_netlog()
// becomes a no-op rather than an error path the caller has to handle.
void orbis_netlog_init(void);

// printf-style; one datagram per call, truncated to 512 bytes.
void orbis_netlog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Non-zero once the socket is open. klog is only a fallback, so callers check this before
// paying 8-15 ms for a klog write.
int  orbis_netlog_ready(void);

void orbis_netlog_close(void);

#ifdef __cplusplus
}
#endif
