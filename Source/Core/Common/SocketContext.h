// Copyright 2021 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef _WIN32
#include <WinSock2.h>
#include <mutex>
#endif

namespace Common
{
#ifdef _WIN32
using SocketHandle = SOCKET;
inline constexpr SocketHandle INVALID_SOCKET_HANDLE = INVALID_SOCKET;
#else
using SocketHandle = int;
inline constexpr SocketHandle INVALID_SOCKET_HANDLE = -1;
#endif

// Winsock ignores select's first argument; POSIX uses the highest descriptor plus one.
constexpr int SelectNfds(SocketHandle socket [[maybe_unused]])
{
#ifdef _WIN32
  return 0;
#else
  return socket + 1;
#endif
}

class SocketContext
{
public:
  SocketContext();
  ~SocketContext();

  SocketContext(const SocketContext&) = delete;
  SocketContext(SocketContext&&) = delete;

  SocketContext& operator=(const SocketContext&) = delete;
  SocketContext& operator=(SocketContext&&) = delete;

private:
#ifdef _WIN32
  static std::mutex s_lock;
  static size_t s_num_objects;
  static WSADATA s_data;
#endif
};
}  // namespace Common
