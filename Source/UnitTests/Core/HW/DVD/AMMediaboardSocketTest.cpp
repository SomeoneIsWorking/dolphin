// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <array>
#include <type_traits>

#include "Core/HW/DVD/AMMediaboardSocket.h"

namespace
{
using AMMediaboard::AcquireMappedSocket;
using AMMediaboard::GuestSocket;
using AMMediaboard::INVALID_GUEST_SOCKET;
using Common::INVALID_SOCKET_HANDLE;
using Common::SocketHandle;

static_assert(std::is_same_v<std::underlying_type_t<GuestSocket>, s32>);

template <typename Acquire>
concept CanAcquireNativeSocket = requires(std::span<SocketHandle> slots, Acquire acquire) {
  AcquireMappedSocket(slots, GuestSocket{1}, acquire, [](SocketHandle) {});
};

static_assert(CanAcquireNativeSocket<SocketHandle (*)()>);
#ifdef _WIN32
static_assert(!CanAcquireNativeSocket<s32 (*)()>);
#endif

TEST(AMMediaboardSocket, NativeIdentitySurvivesAcquisitionAndMapping)
{
  std::array<SocketHandle, 4> slots;
  slots.fill(INVALID_SOCKET_HANDLE);
#ifdef _WIN32
  constexpr SocketHandle first = (SocketHandle{1} << 63) | 7;
  constexpr SocketHandle second = (SocketHandle{1} << 40) | 7;
#else
  constexpr SocketHandle first = 0x40000007;
  constexpr SocketHandle second = 7;
#endif
  SocketHandle configured = INVALID_SOCKET_HANDLE;
  int configurations = 0;
  const auto configure = [&](SocketHandle handle) {
    configured = handle;
    ++configurations;
  };
  EXPECT_EQ(
      AcquireMappedSocket(slots, GuestSocket{1}, [] { return first; }, configure), GuestSocket{1});
  EXPECT_EQ(configured, first);
  EXPECT_EQ(slots[1], first);
  EXPECT_EQ(
      AcquireMappedSocket(slots, GuestSocket{2}, [] { return second; }, configure), GuestSocket{2});
  EXPECT_EQ(configured, second);
  EXPECT_EQ(slots[2], second);
  EXPECT_NE(slots[1], slots[2]);
  EXPECT_EQ(configurations, 2);
}

TEST(AMMediaboardSocket, FailedNativeAcquisitionNeverPublishesOrConfigures)
{
  std::array<SocketHandle, 4> slots;
  slots.fill(INVALID_SOCKET_HANDLE);
  const auto before = slots;
  int acquisitions = 0;
  int configurations = 0;
  EXPECT_EQ(AcquireMappedSocket(
                slots, GuestSocket{1},
                [&] {
                  ++acquisitions;
                  return INVALID_SOCKET_HANDLE;
                },
                [&](SocketHandle) { ++configurations; }),
            INVALID_GUEST_SOCKET);
  EXPECT_EQ(slots, before);
  EXPECT_EQ(acquisitions, 1);
  EXPECT_EQ(configurations, 0);
}

TEST(AMMediaboardSocket, UnavailableGuestSlotNeverAcquiresNativeResource)
{
  std::array<SocketHandle, 4> slots;
  slots.fill(INVALID_SOCKET_HANDLE);
  slots[1] = SocketHandle{17};
  const auto before = slots;
  int acquisitions = 0;
  int configurations = 0;
  for (const GuestSocket guest : {INVALID_GUEST_SOCKET, GuestSocket{4}, GuestSocket{1}})
  {
    EXPECT_EQ(AcquireMappedSocket(
                  slots, guest,
                  [&] {
                    ++acquisitions;
                    return SocketHandle{23};
                  },
                  [&](SocketHandle) { ++configurations; }),
              INVALID_GUEST_SOCKET);
  }
  EXPECT_EQ(slots, before);
  EXPECT_EQ(acquisitions, 0);
  EXPECT_EQ(configurations, 0);
}
}  // namespace
