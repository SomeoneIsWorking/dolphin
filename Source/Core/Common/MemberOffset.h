// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>

namespace Common
{
// JIT state may have a non-standard layout. Measure a real subobject rather than using offsetof
// or forming a member through a null pointer. The caller must supply the state the JIT addresses.
template <typename Object, typename Member>
std::size_t MemberOffset(const Object& object, const Member& member)
{
  const auto base = reinterpret_cast<std::uintptr_t>(std::addressof(object));
  const auto address = reinterpret_cast<std::uintptr_t>(std::addressof(member));
  if constexpr (sizeof(Member) > sizeof(Object))
  {
    std::abort();
  }
  else if (address < base || address - base > sizeof(Object) - sizeof(Member))
  {
    std::abort();
  }
  return address - base;
}
}  // namespace Common
