#pragma once

// LLVM recognizes the FreeBSD compiler triple, while OpenOrbis exposes the
// same byte-order constants through musl's endian.h.
#include <endian.h>
