// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <type_traits>

#include "Common/MemberOffset.h"

namespace
{
class NonStandardState
{
public:
  std::array<std::uint64_t, 4> registers{};
  [[nodiscard]] const auto& GetStatus() const { return m_status; }

private:
  std::uint32_t m_status = 0;
};
static_assert(!std::is_standard_layout_v<NonStandardState>);
}  // namespace

TEST(MemberOffset, AddressesLiveNonStandardSubobjects)
{
  const NonStandardState state;
  const auto base = reinterpret_cast<std::uintptr_t>(std::addressof(state));
  for (const auto& reg : state.registers)
  {
    EXPECT_EQ(base + Common::MemberOffset(state, reg),
              reinterpret_cast<std::uintptr_t>(std::addressof(reg)));
  }
  EXPECT_EQ(base + Common::MemberOffset(state, state.GetStatus()),
            reinterpret_cast<std::uintptr_t>(std::addressof(state.GetStatus())));
}

TEST(MemberOffsetDeathTest, RejectsForeignSubobject)
{
  const NonStandardState states[2];
  EXPECT_DEATH(Common::MemberOffset(states[0], states[1].registers[0]), "");
}
