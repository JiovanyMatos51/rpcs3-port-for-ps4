// Copyright © 2026 Mikołaj Mikołajczyk
// SPDX-License-Identifier: MIT
//
// getenv() for an image that is not the one the env file was applied to.
//
// Use this instead of getenv() for any knob a loadable module might read. The SDK's libc is a
// static musl archive, so the executable and every .prx it loads have their own `environ`;
// setenv() in one is invisible to the others (see src/orbis_env.cpp).
//
// The process's own environment is checked first, so anything genuinely set still wins.
#ifndef ORBIS_ENV_H
#define ORBIS_ENV_H

#ifdef __cplusplus
extern "C" {
#endif

const char* orbis_env_get(const char* name);

#ifdef __cplusplus
}
#endif

#endif  // ORBIS_ENV_H
