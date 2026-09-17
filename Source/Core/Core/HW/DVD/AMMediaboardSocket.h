// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <concepts>
#include <functional>
#include <span>
#include <type_traits>

#include "Common/CommonTypes.h"
#include "Common/SocketContext.h"

namespace AMMediaboard
{
enum class GuestSocket : s32
{
};
inline constexpr auto INVALID_GUEST_SOCKET = GuestSocket(-1);

// A guest slot is chosen before the OS operation so allocation failure cannot leak a socket.
// Requiring the native return type prevents either socket() or accept() from narrowing first.
template <typename Acquire, typename Configure>
requires std::same_as<std::invoke_result_t<Acquire>, Common::SocketHandle>
GuestSocket AcquireMappedSocket(std::span<Common::SocketHandle> sockets, GuestSocket guest,
                                Acquire acquire, Configure configure)
{
  const auto index = static_cast<std::size_t>(guest);
  if (index >= sockets.size() || sockets[index] != Common::INVALID_SOCKET_HANDLE)
    return INVALID_GUEST_SOCKET;

  const auto host = std::invoke(acquire);
  if (host == Common::INVALID_SOCKET_HANDLE)
    return INVALID_GUEST_SOCKET;

  std::invoke(configure, host);
  sockets[index] = host;
  return guest;
}
}  // namespace AMMediaboard
