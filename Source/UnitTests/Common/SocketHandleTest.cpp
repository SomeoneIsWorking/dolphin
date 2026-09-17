// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <limits>
#include <type_traits>

#ifndef _WIN32
#include <sys/select.h>
#endif

#include "Common/SocketContext.h"

TEST(SocketHandle, NativeDescriptorIdentity)
{
  using Common::SocketHandle;
  fd_set descriptors;
  FD_ZERO(&descriptors);
#ifdef _WIN32
  static_assert(std::is_same_v<SocketHandle, SOCKET>);
  // Distinct handles with the same low 32 bits must not alias through an int descriptor.
  static_assert(sizeof(SocketHandle) == 8);
  constexpr SocketHandle first = (SocketHandle{1} << 40) | 7;
  constexpr SocketHandle second = (SocketHandle{2} << 40) | 7;
  FD_SET(first, &descriptors);
  EXPECT_NE(FD_ISSET(first, &descriptors), 0);
  EXPECT_EQ(FD_ISSET(second, &descriptors), 0);
  FD_SET(second, &descriptors);
  EXPECT_EQ(descriptors.fd_count, 2u);
  EXPECT_EQ(descriptors.fd_array[0], first);
  EXPECT_EQ(descriptors.fd_array[1], second);
  EXPECT_EQ(Common::INVALID_SOCKET_HANDLE, INVALID_SOCKET);
  EXPECT_EQ(Common::SelectNfds(first), 0);
  EXPECT_EQ(Common::SelectNfds(std::numeric_limits<SocketHandle>::max()), 0);
#else
  static_assert(std::is_same_v<SocketHandle, int>);
  constexpr SocketHandle first = 7;
  constexpr SocketHandle second = 8;
  FD_SET(first, &descriptors);
  EXPECT_NE(FD_ISSET(first, &descriptors), 0);
  EXPECT_EQ(FD_ISSET(second, &descriptors), 0);
  EXPECT_EQ(Common::INVALID_SOCKET_HANDLE, -1);
  EXPECT_EQ(Common::SelectNfds(first), 8);
  EXPECT_EQ(Common::SelectNfds(second), 9);
#endif
}
