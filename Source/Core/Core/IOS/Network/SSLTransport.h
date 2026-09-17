// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "Common/SocketContext.h"

namespace IOS::HLE::SSLTransport
{
// The Wii socket owner configures nonblocking I/O. These callbacks preserve native socket
// identity and return mbedTLS transport statuses without using its int-sized net context ABI.
int Send(Common::SocketHandle socket, const unsigned char* data, std::size_t length);
int Receive(Common::SocketHandle socket, unsigned char* data, std::size_t length);
}  // namespace IOS::HLE::SSLTransport
