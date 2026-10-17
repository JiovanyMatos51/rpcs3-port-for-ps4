#pragma once
#include <stdint.h>
#define _TIMEVAL_DEFINED
// Model the console ABI while testing the adapter on Windows.
struct timeval { int64_t tv_sec; int64_t tv_usec; };
