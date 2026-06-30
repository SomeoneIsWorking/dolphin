// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/GPFifo.h"

#include <cstddef>
#include <cstring>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/SunbrightHooks.h"
#include "Common/Swap.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "VideoCommon/CommandProcessor.h"

namespace GPFifo
{
GPFifoManager::GPFifoManager(Core::System& system) : m_system(system)
{
}

// 32 Byte gather pipe with extra space
// Overfilling is no problem (up to the real limit), CheckGatherPipe will blast the
// contents in nicely sized chunks
//
// Other optimizations to think about:
// - If the GP is NOT linked to the FIFO, just blast to memory byte by word
// - If the GP IS linked to the FIFO, use a fast wrapping buffer and skip writing to memory
//
// Both of these should actually work! Only problem is that we have to decide at run time,
// the same function could use both methods. Compile 2 different versions of each such block?

size_t GPFifoManager::GetGatherPipeCount()
{
  return m_system.GetPPCState().gather_pipe_ptr - m_gather_pipe;
}

void GPFifoManager::SetGatherPipeCount(size_t size)
{
  m_system.GetPPCState().gather_pipe_ptr = m_gather_pipe + size;
}

void GPFifoManager::DoState(PointerWrap& p)
{
  p.Do(m_gather_pipe);
  u32 pipe_count = static_cast<u32>(GetGatherPipeCount());
  p.Do(pipe_count);
  SetGatherPipeCount(pipe_count);
}

void GPFifoManager::Init()
{
  ResetGatherPipe();
  m_system.GetPPCState().gather_pipe_base_ptr = m_gather_pipe;
  memset(m_gather_pipe, 0, sizeof(m_gather_pipe));
}

bool GPFifoManager::IsBNE() const
{
  // TODO: It's not clear exactly when the BNE (buffer not empty) bit is set.
  // The PPC 750cl manual says in section 2.1.2.12 "Write Pipe Address Register (WPAR)" (page 78):
  // "A mfspr WPAR is used to read the BNE bit to check for any outstanding data transfers."
  // In section 9.4.2 "Write Gather Pipe Operation" (page 327) it says:
  // "Software can check WPAR[BNE] to determine if the buffer is empty or not."
  // On page 327, it also says "The only way for software to flush out a partially full 32 byte
  // block is to fill up the block with dummy data,."
  // On page 328, it says: "Before disabling the write gather pipe, the WPAR[BNE] bit should be
  // tested to insure that all outstanding transfers from the buffer to the bus have completed."
  //
  // GXRedirectWriteGatherPipe and GXRestoreWriteGatherPipe (used for display lists) wait for
  // the bit to be 0 before continuing, so it can't be a case of any data existing in the FIFO;
  // it might be a case of over 32 bytes being stored pending transfer to memory? For now, always
  // return false since that prevents hangs in games that use display lists.
  return false;
}

void GPFifoManager::ResetGatherPipe()
{
  SetGatherPipeCount(0);
}

void GPFifoManager::UpdateGatherPipe()
{
  auto& system = m_system;
  auto& memory = system.GetMemory();
  auto& processor_interface = system.GetProcessorInterface();

  size_t pipe_count = GetGatherPipeCount();
  size_t processed;
  for (processed = 0; pipe_count >= GATHER_PIPE_SIZE; processed += GATHER_PIPE_SIZE)
  {
    // copy the GatherPipe
    memory.CopyToEmu(processor_interface.m_fifo_cpu_write_pointer, m_gather_pipe + processed,
                     GATHER_PIPE_SIZE);
    // Sunbright: tee every gather-pipe chunk to the GX-command-stream parity oracle. This is the
    // ONLY point that sees the JIT inline-gather fast path too (it bypasses Write*). Capture-only.
    if (sb_slot_gather_flush)
      sb_slot_gather_flush(m_gather_pipe + processed, GATHER_PIPE_SIZE);
    pipe_count -= GATHER_PIPE_SIZE;

    // increase the CPUWritePointer
    if (processor_interface.m_fifo_cpu_write_pointer == processor_interface.m_fifo_cpu_end)
      processor_interface.m_fifo_cpu_write_pointer = processor_interface.m_fifo_cpu_base;
    else
      processor_interface.m_fifo_cpu_write_pointer += GATHER_PIPE_SIZE;

    system.GetCommandProcessor().GatherPipeBursted();
  }

  // move back the spill bytes
  memmove(m_gather_pipe, m_gather_pipe + processed, pipe_count);
  SetGatherPipeCount(pipe_count);
}

void GPFifoManager::FastCheckGatherPipe()
{
  if (GetGatherPipeCount() >= GATHER_PIPE_SIZE)
  {
    UpdateGatherPipe();
  }
}

void GPFifoManager::CheckGatherPipe()
{
  if (GetGatherPipeCount() >= GATHER_PIPE_SIZE)
  {
    UpdateGatherPipe();

    // Profile where slow FIFO writes are occurring.
    m_system.GetJitInterface().CompileExceptionCheck(JitInterface::ExceptionType::FIFOWrite);
  }
}

// Sunbright hooks: foreign gather-pipe write funnel (runtime/gpfifo_wrap.cpp). When installed, a
// hook routes the write into the held GX stream while the assembler is armed, else calls the
// matching *_impl (the original body). Default-null = original behavior.
void GPFifoManager::Write8(const u8 value)
{
  if (sb_slot_gpfifo_write8) { sb_slot_gpfifo_write8(this, value); return; }
  sb_gpfifo_write8_impl(this, value);
}

void GPFifoManager::Write16(const u16 value)
{
  if (sb_slot_gpfifo_write16) { sb_slot_gpfifo_write16(this, value); return; }
  sb_gpfifo_write16_impl(this, value);
}

void GPFifoManager::Write32(const u32 value)
{
  if (sb_slot_gpfifo_write32) { sb_slot_gpfifo_write32(this, value); return; }
  sb_gpfifo_write32_impl(this, value);
}

void GPFifoManager::Write64(const u64 value)
{
  if (sb_slot_gpfifo_write64) { sb_slot_gpfifo_write64(this, value); return; }
  sb_gpfifo_write64_impl(this, value);
}

void GPFifoManager::FastWrite8(const u8 value)
{
  auto& ppc_state = m_system.GetPPCState();
  *ppc_state.gather_pipe_ptr = value;
  ppc_state.gather_pipe_ptr += sizeof(u8);
}

void GPFifoManager::FastWrite16(u16 value)
{
  value = Common::swap16(value);
  auto& ppc_state = m_system.GetPPCState();
  std::memcpy(ppc_state.gather_pipe_ptr, &value, sizeof(u16));
  ppc_state.gather_pipe_ptr += sizeof(u16);
}

void GPFifoManager::FastWrite32(u32 value)
{
  value = Common::swap32(value);
  auto& ppc_state = m_system.GetPPCState();
  std::memcpy(ppc_state.gather_pipe_ptr, &value, sizeof(u32));
  ppc_state.gather_pipe_ptr += sizeof(u32);
}

void GPFifoManager::FastWrite64(u64 value)
{
  value = Common::swap64(value);
  auto& ppc_state = m_system.GetPPCState();
  std::memcpy(ppc_state.gather_pipe_ptr, &value, sizeof(u64));
  ppc_state.gather_pipe_ptr += sizeof(u64);
}

void UpdateGatherPipe(GPFifoManager& gpfifo)
{
  gpfifo.UpdateGatherPipe();
}

void FastCheckGatherPipe(GPFifoManager& gpfifo)
{
  gpfifo.FastCheckGatherPipe();
}
}  // namespace GPFifo

// ── Sunbright hook plumbing (replaces linker --wrap on GPFifoManager::Write*) ─────────────────
// Original bodies, callable directly by the Sunbright hook in place of __real_.
extern "C" void sb_gpfifo_write8_impl(void* self, u8 v)
{
  auto* g = static_cast<GPFifo::GPFifoManager*>(self);
  g->FastWrite8(v);
  g->CheckGatherPipe();
}
extern "C" void sb_gpfifo_write16_impl(void* self, u16 v)
{
  auto* g = static_cast<GPFifo::GPFifoManager*>(self);
  g->FastWrite16(v);
  g->CheckGatherPipe();
}
extern "C" void sb_gpfifo_write32_impl(void* self, u32 v)
{
  auto* g = static_cast<GPFifo::GPFifoManager*>(self);
  g->FastWrite32(v);
  g->CheckGatherPipe();
}
extern "C" void sb_gpfifo_write64_impl(void* self, u64 v)
{
  auto* g = static_cast<GPFifo::GPFifoManager*>(self);
  g->FastWrite64(v);
  g->CheckGatherPipe();
}
// Hook slots (default null = original behavior). Set by sb_install_hooks().
extern "C" void (*sb_slot_gpfifo_write8)(void*, u8) = nullptr;
extern "C" void (*sb_slot_gpfifo_write16)(void*, u16) = nullptr;
extern "C" void (*sb_slot_gpfifo_write32)(void*, u32) = nullptr;
extern "C" void (*sb_slot_gpfifo_write64)(void*, u64) = nullptr;
extern "C" void (*sb_slot_gather_flush)(const u8*, std::size_t) = nullptr;
