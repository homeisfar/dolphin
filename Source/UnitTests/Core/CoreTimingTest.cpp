// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <array>
#include <bitset>
#include <string>

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "UICommon/UICommon.h"

// Numbers are chosen randomly to make sure the correct one is given.
static constexpr std::array<u64, 5> CB_IDS{{42, 144, 93, 1026, UINT64_C(0xFFFF7FFFF7FFFF)}};
static constexpr int MAX_SLICE_LENGTH = 20000;  // Copied from CoreTiming internals

static std::bitset<CB_IDS.size()> s_callbacks_ran_flags;
static u64 s_expected_callback = 0;
static s64 s_lateness = 0;

template <unsigned int IDX>
void CallbackTemplate(Core::System& system, u64 userdata, s64 lateness)
{
  static_assert(IDX < CB_IDS.size(), "IDX out of range");
  s_callbacks_ran_flags.set(IDX);
  EXPECT_EQ(CB_IDS[IDX], userdata);
  EXPECT_EQ(CB_IDS[IDX], s_expected_callback);
  EXPECT_EQ(s_lateness, lateness);
}

class ScopeInit final
{
public:
  ScopeInit() : m_profile_path(File::CreateTempDir())
  {
    if (!UserDirectoryExists())
    {
      return;
    }
    Core::DeclareAsCPUThread();
    UICommon::SetUserDirectory(m_profile_path);
    Config::Init();
    SConfig::Init();
    PowerPC::Init(PowerPC::CPUCore::Interpreter);
    auto& system = Core::System::GetInstance();
    auto& core_timing = system.GetCoreTiming();
    core_timing.Init();
  }
  ~ScopeInit()
  {
    if (!UserDirectoryExists())
    {
      return;
    }
    auto& system = Core::System::GetInstance();
    auto& core_timing = system.GetCoreTiming();
    core_timing.Shutdown();
    PowerPC::Shutdown();
    SConfig::Shutdown();
    Config::Shutdown();
    Core::UndeclareAsCPUThread();
    File::DeleteDirRecursively(m_profile_path);
  }
  bool UserDirectoryExists() const { return !m_profile_path.empty(); }

private:
  std::string m_profile_path;
};

static void AdvanceAndCheck(u32 idx, int downcount, int expected_lateness = 0,
                            int cpu_downcount = 0)
{
  s_callbacks_ran_flags = 0;
  s_expected_callback = CB_IDS[idx];
  s_lateness = expected_lateness;

  PowerPC::ppcState.downcount = cpu_downcount;  // Pretend we executed X cycles of instructions.
  auto& system = Core::System::GetInstance();
  auto& core_timing = system.GetCoreTiming();
  core_timing.Advance();

  EXPECT_EQ(decltype(s_callbacks_ran_flags)().set(idx), s_callbacks_ran_flags);
  EXPECT_EQ(downcount, PowerPC::ppcState.downcount);
}

TEST(CoreTiming, BasicOrder)
{
  ScopeInit guard;
  ASSERT_TRUE(guard.UserDirectoryExists());

  auto& system = Core::System::GetInstance();
  auto& core_timing = system.GetCoreTiming();

  CoreTiming::EventType* cb_a = core_timing.RegisterEvent("callbackA", CallbackTemplate<0>);
  CoreTiming::EventType* cb_b = core_timing.RegisterEvent("callbackB", CallbackTemplate<1>);
  CoreTiming::EventType* cb_c = core_timing.RegisterEvent("callbackC", CallbackTemplate<2>);
  CoreTiming::EventType* cb_d = core_timing.RegisterEvent("callbackD", CallbackTemplate<3>);
  CoreTiming::EventType* cb_e = core_timing.RegisterEvent("callbackE", CallbackTemplate<4>);

  // Enter slice 0
  core_timing.Advance();

  // D -> B -> C -> A -> E
  core_timing.ScheduleEvent(1000, cb_a, CB_IDS[0]);
  EXPECT_EQ(1000, PowerPC::ppcState.downcount);
  core_timing.ScheduleEvent(500, cb_b, CB_IDS[1]);
  EXPECT_EQ(500, PowerPC::ppcState.downcount);
  core_timing.ScheduleEvent(800, cb_c, CB_IDS[2]);
  EXPECT_EQ(500, PowerPC::ppcState.downcount);
  core_timing.ScheduleEvent(100, cb_d, CB_IDS[3]);
  EXPECT_EQ(100, PowerPC::ppcState.downcount);
  core_timing.ScheduleEvent(1200, cb_e, CB_IDS[4]);
  EXPECT_EQ(100, PowerPC::ppcState.downcount);

  AdvanceAndCheck(3, 400);
  AdvanceAndCheck(1, 300);
  AdvanceAndCheck(2, 200);
  AdvanceAndCheck(0, 200);
  AdvanceAndCheck(4, MAX_SLICE_LENGTH);
}

namespace SharedSlotTest
{
static unsigned int s_counter = 0;

template <unsigned int ID>
void FifoCallback(Core::System& system, u64 userdata, s64 lateness)
{
  static_assert(ID < CB_IDS.size(), "ID out of range");
  s_callbacks_ran_flags.set(ID);
  EXPECT_EQ(CB_IDS[ID], userdata);
  EXPECT_EQ(ID, s_counter);
  EXPECT_EQ(s_lateness, lateness);
  ++s_counter;
}
}  // namespace SharedSlotTest

TEST(CoreTiming, SharedSlot)
{
  using namespace SharedSlotTest;

  ScopeInit guard;
  ASSERT_TRUE(guard.UserDirectoryExists());

  auto& system = Core::System::GetInstance();
  auto& core_timing = system.GetCoreTiming();

  CoreTiming::EventType* cb_a = core_timing.RegisterEvent("callbackA", FifoCallback<0>);
  CoreTiming::EventType* cb_b = core_timing.RegisterEvent("callbackB", FifoCallback<1>);
  CoreTiming::EventType* cb_c = core_timing.RegisterEvent("callbackC", FifoCallback<2>);
  CoreTiming::EventType* cb_d = core_timing.RegisterEvent("callbackD", FifoCallback<3>);
  CoreTiming::EventType* cb_e = core_timing.RegisterEvent("callbackE", FifoCallback<4>);

  core_timing.ScheduleEvent(1000, cb_a, CB_IDS[0]);
  core_timing.ScheduleEvent(1000, cb_b, CB_IDS[1]);
  core_timing.ScheduleEvent(1000, cb_c, CB_IDS[2]);
  core_timing.ScheduleEvent(1000, cb_d, CB_IDS[3]);
  core_timing.ScheduleEvent(1000, cb_e, CB_IDS[4]);

  // Enter slice 0
  core_timing.Advance();
  EXPECT_EQ(1000, PowerPC::ppcState.downcount);

  s_callbacks_ran_flags = 0;
  s_counter = 0;
  s_lateness = 0;
  PowerPC::ppcState.downcount = 0;
  core_timing.Advance();
  EXPECT_EQ(MAX_SLICE_LENGTH, PowerPC::ppcState.downcount);
  EXPECT_EQ(0x1FULL, s_callbacks_ran_flags.to_ullong());
}

TEST(CoreTiming, PredictableLateness)
{
  ScopeInit guard;
  ASSERT_TRUE(guard.UserDirectoryExists());

  auto& system = Core::System::GetInstance();
  auto& core_timing = system.GetCoreTiming();

  CoreTiming::EventType* cb_a = core_timing.RegisterEvent("callbackA", CallbackTemplate<0>);
  CoreTiming::EventType* cb_b = core_timing.RegisterEvent("callbackB", CallbackTemplate<1>);

  // Enter slice 0
  core_timing.Advance();

  core_timing.ScheduleEvent(100, cb_a, CB_IDS[0]);
  core_timing.ScheduleEvent(200, cb_b, CB_IDS[1]);

  AdvanceAndCheck(0, 90, 10, -10);  // (100 - 10)
  AdvanceAndCheck(1, MAX_SLICE_LENGTH, 50, -50);
}

namespace ChainSchedulingTest
{
static int s_reschedules = 0;

static void RescheduleCallback(Core::System& system, u64 userdata, s64 lateness)
{
  --s_reschedules;
  EXPECT_TRUE(s_reschedules >= 0);
  EXPECT_EQ(s_lateness, lateness);

  if (s_reschedules > 0)
  {
    system.GetCoreTiming().ScheduleEvent(1000, reinterpret_cast<CoreTiming::EventType*>(userdata),
                                         userdata);
  }
}
}  // namespace ChainSchedulingTest

TEST(CoreTiming, ChainScheduling)
{
  using namespace ChainSchedulingTest;

  ScopeInit guard;
  ASSERT_TRUE(guard.UserDirectoryExists());

  auto& system = Core::System::GetInstance();
  auto& core_timing = system.GetCoreTiming();

  CoreTiming::EventType* cb_a = core_timing.RegisterEvent("callbackA", CallbackTemplate<0>);
  CoreTiming::EventType* cb_b = core_timing.RegisterEvent("callbackB", CallbackTemplate<1>);
  CoreTiming::EventType* cb_c = core_timing.RegisterEvent("callbackC", CallbackTemplate<2>);
  CoreTiming::EventType* cb_rs =
      core_timing.RegisterEvent("callbackReschedule", RescheduleCallback);

  // Enter slice 0
  core_timing.Advance();

  core_timing.ScheduleEvent(800, cb_a, CB_IDS[0]);
  core_timing.ScheduleEvent(1000, cb_b, CB_IDS[1]);
  core_timing.ScheduleEvent(2200, cb_c, CB_IDS[2]);
  core_timing.ScheduleEvent(1000, cb_rs, reinterpret_cast<u64>(cb_rs));
  EXPECT_EQ(800, PowerPC::ppcState.downcount);

  s_reschedules = 3;
  AdvanceAndCheck(0, 200);   // cb_a
  AdvanceAndCheck(1, 1000);  // cb_b, cb_rs
  EXPECT_EQ(2, s_reschedules);

  PowerPC::ppcState.downcount = 0;
  core_timing.Advance();  // cb_rs
  EXPECT_EQ(1, s_reschedules);
  EXPECT_EQ(200, PowerPC::ppcState.downcount);

  AdvanceAndCheck(2, 800);  // cb_c

  PowerPC::ppcState.downcount = 0;
  core_timing.Advance();  // cb_rs
  EXPECT_EQ(0, s_reschedules);
  EXPECT_EQ(MAX_SLICE_LENGTH, PowerPC::ppcState.downcount);
}

namespace ScheduleIntoPastTest
{
static CoreTiming::EventType* s_cb_next = nullptr;

static void ChainCallback(Core::System& system, u64 userdata, s64 lateness)
{
  EXPECT_EQ(CB_IDS[0] + 1, userdata);
  EXPECT_EQ(0, lateness);

  system.GetCoreTiming().ScheduleEvent(-1000, s_cb_next, userdata - 1);
}
}  // namespace ScheduleIntoPastTest

// This can happen when scheduling from outside the CPU Thread.
// Also, if the callback is very late, it may reschedule itself for the next period which
// is also in the past.
TEST(CoreTiming, ScheduleIntoPast)
{
  using namespace ScheduleIntoPastTest;

  ScopeInit guard;
  ASSERT_TRUE(guard.UserDirectoryExists());

  auto& system = Core::System::GetInstance();
  auto& core_timing = system.GetCoreTiming();

  s_cb_next = core_timing.RegisterEvent("callbackA", CallbackTemplate<0>);
  CoreTiming::EventType* cb_b = core_timing.RegisterEvent("callbackB", CallbackTemplate<1>);
  CoreTiming::EventType* cb_chain = core_timing.RegisterEvent("callbackChain", ChainCallback);

  // Enter slice 0
  core_timing.Advance();

  core_timing.ScheduleEvent(1000, cb_chain, CB_IDS[0] + 1);
  EXPECT_EQ(1000, PowerPC::ppcState.downcount);

  AdvanceAndCheck(0, MAX_SLICE_LENGTH, 1000);  // Run cb_chain into late cb_a

  // Schedule late from wrong thread
  // The problem with scheduling CPU events from outside the CPU Thread is that g_global_timer
  // is not reliable outside the CPU Thread. It's possible for the other thread to sample the
  // global timer right before the timer is updated by Advance() then submit a new event using
  // the stale value, i.e. effectively half-way through the previous slice.
  // NOTE: We're only testing that the scheduler doesn't break, not whether this makes sense.
  Core::UndeclareAsCPUThread();
  auto& core_timing_globals = Core::System::GetInstance().GetCoreTimingGlobals();
  core_timing_globals.global_timer -= 1000;
  core_timing.ScheduleEvent(0, cb_b, CB_IDS[1], CoreTiming::FromThread::NON_CPU);
  core_timing_globals.global_timer += 1000;
  Core::DeclareAsCPUThread();
  AdvanceAndCheck(1, MAX_SLICE_LENGTH, MAX_SLICE_LENGTH + 1000);

  // Schedule directly into the past from the CPU.
  // This shouldn't happen in practice, but it's best if we don't mess up the slice length and
  // downcount if we do.
  core_timing.ScheduleEvent(-1000, s_cb_next, CB_IDS[0]);
  EXPECT_EQ(0, PowerPC::ppcState.downcount);
  AdvanceAndCheck(0, MAX_SLICE_LENGTH, 1000);
}

TEST(CoreTiming, Overclocking)
{
  ScopeInit guard;
  ASSERT_TRUE(guard.UserDirectoryExists());

  auto& system = Core::System::GetInstance();
  auto& core_timing = system.GetCoreTiming();

  CoreTiming::EventType* cb_a = core_timing.RegisterEvent("callbackA", CallbackTemplate<0>);
  CoreTiming::EventType* cb_b = core_timing.RegisterEvent("callbackB", CallbackTemplate<1>);
  CoreTiming::EventType* cb_c = core_timing.RegisterEvent("callbackC", CallbackTemplate<2>);
  CoreTiming::EventType* cb_d = core_timing.RegisterEvent("callbackD", CallbackTemplate<3>);
  CoreTiming::EventType* cb_e = core_timing.RegisterEvent("callbackE", CallbackTemplate<4>);

  // Overclock
  Config::SetCurrent(Config::MAIN_OVERCLOCK_ENABLE, true);
  Config::SetCurrent(Config::MAIN_OVERCLOCK, 2.0f);

  // Enter slice 0
  // Updates s_last_OC_factor.
  core_timing.Advance();

  core_timing.ScheduleEvent(100, cb_a, CB_IDS[0]);
  core_timing.ScheduleEvent(200, cb_b, CB_IDS[1]);
  core_timing.ScheduleEvent(400, cb_c, CB_IDS[2]);
  core_timing.ScheduleEvent(800, cb_d, CB_IDS[3]);
  core_timing.ScheduleEvent(1600, cb_e, CB_IDS[4]);
  EXPECT_EQ(200, PowerPC::ppcState.downcount);

  AdvanceAndCheck(0, 200);   // (200 - 100) * 2
  AdvanceAndCheck(1, 400);   // (400 - 200) * 2
  AdvanceAndCheck(2, 800);   // (800 - 400) * 2
  AdvanceAndCheck(3, 1600);  // (1600 - 800) * 2
  AdvanceAndCheck(4, MAX_SLICE_LENGTH * 2);

  // Underclock
  Config::SetCurrent(Config::MAIN_OVERCLOCK, 0.5f);
  core_timing.Advance();

  core_timing.ScheduleEvent(100, cb_a, CB_IDS[0]);
  core_timing.ScheduleEvent(200, cb_b, CB_IDS[1]);
  core_timing.ScheduleEvent(400, cb_c, CB_IDS[2]);
  core_timing.ScheduleEvent(800, cb_d, CB_IDS[3]);
  core_timing.ScheduleEvent(1600, cb_e, CB_IDS[4]);
  EXPECT_EQ(50, PowerPC::ppcState.downcount);

  AdvanceAndCheck(0, 50);   // (200 - 100) / 2
  AdvanceAndCheck(1, 100);  // (400 - 200) / 2
  AdvanceAndCheck(2, 200);  // (800 - 400) / 2
  AdvanceAndCheck(3, 400);  // (1600 - 800) / 2
  AdvanceAndCheck(4, MAX_SLICE_LENGTH / 2);

  // Try switching the clock mid-emulation
  Config::SetCurrent(Config::MAIN_OVERCLOCK, 1.0f);
  core_timing.Advance();

  core_timing.ScheduleEvent(100, cb_a, CB_IDS[0]);
  core_timing.ScheduleEvent(200, cb_b, CB_IDS[1]);
  core_timing.ScheduleEvent(400, cb_c, CB_IDS[2]);
  core_timing.ScheduleEvent(800, cb_d, CB_IDS[3]);
  core_timing.ScheduleEvent(1600, cb_e, CB_IDS[4]);
  EXPECT_EQ(100, PowerPC::ppcState.downcount);

  AdvanceAndCheck(0, 100);  // (200 - 100)
  Config::SetCurrent(Config::MAIN_OVERCLOCK, 2.0f);
  AdvanceAndCheck(1, 400);  // (400 - 200) * 2
  AdvanceAndCheck(2, 800);  // (800 - 400) * 2
  Config::SetCurrent(Config::MAIN_OVERCLOCK, 0.1f);
  AdvanceAndCheck(3, 80);  // (1600 - 800) / 10
  Config::SetCurrent(Config::MAIN_OVERCLOCK, 1.0f);
  AdvanceAndCheck(4, MAX_SLICE_LENGTH);
}
