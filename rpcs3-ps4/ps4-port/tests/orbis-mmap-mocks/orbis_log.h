#pragma once

// Host stand-in for orbis-compat's log sink (orbis_mmap_trim_test.cpp defines it).
extern "C" void orbis_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
extern "C" int orbis_log_enabled(void);
