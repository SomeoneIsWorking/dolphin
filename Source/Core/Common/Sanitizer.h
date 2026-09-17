// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Intentional fault probes must reach the host exception handler without ASan instrumentation.
// clang-cl defines _MSC_VER, but uses Clang's sanitizer attribute rather than MSVC's declspec.
#if defined(__clang__)
#define DOLPHIN_NO_SANITIZE_ADDRESS __attribute__((no_sanitize("address")))
#elif defined(__GNUC__)
#define DOLPHIN_NO_SANITIZE_ADDRESS __attribute__((no_sanitize_address))
#elif defined(_MSC_VER)
#define DOLPHIN_NO_SANITIZE_ADDRESS __declspec(no_sanitize_address)
#else
#define DOLPHIN_NO_SANITIZE_ADDRESS
#endif
