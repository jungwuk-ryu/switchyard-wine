// SPDX-License-Identifier: MIT
// Actual-core stopped-state/ownership differential; not a performance score.
#include "../src/switchyard_fex.cpp"
#include "register_window_reference.h"
#include <array>
#include <source_location>

static void Check(bool Value, const std::source_location Site = std::source_location::current()) {
  if (Value) return;
  std::fprintf(stderr, "completion whole-state line %u\n", Site.line());
  std::_Exit(97);
}
static void Require(switchyard_fex_result Result) { Check(Result == SWITCHYARD_FEX_OK); }
struct WakeProof {
  std::mutex Mutex;
  switchyard_fex_admission* Cell {};
  const unsigned char* Output {};
  const unsigned char* Expected {};
  unsigned Wakes {};
};
static void Wake(void* Context) {
  auto& Proof = *static_cast<WakeProof*>(Context);
  Check((switchyard_fex_admission_load(Proof.Cell) & SWITCHYARD_FEX_ADMISSION_FLAGS) ==
        SWITCHYARD_FEX_ADMISSION_CLOSED);
  std::lock_guard Lock(Proof.Mutex);
  Check(std::memcmp(Proof.Output, Proof.Expected, 408) == 0);
  ++Proof.Wakes;
}
static switchyard_fex_stop StopValue() {
  switchyard_fex_stop Result {};
  Result.size = sizeof(Result);
  Result.version = SWITCHYARD_FEX_STOP_VERSION;
  return Result;
}
static switchyard_fex_register_window Window(void* Data) {
  return {sizeof(switchyard_fex_register_window), SWITCHYARD_FEX_REGISTER_WINDOW_VERSION,
          0, 0, reinterpret_cast<uintptr_t>(Data), 408, 0};
}

int main() {
  WakeProof Proof;
  switchyard_fex_config Config {};
  Config.size = sizeof(Config);
  Config.abi_version = SWITCHYARD_FEX_ABI_VERSION;
  Config.flags = SWITCHYARD_FEX_CONFIG_EXTERNAL_ADMISSION;
  switchyard_fex_execution_domain Domain {sizeof(Domain), SWITCHYARD_FEX_DOMAIN_VERSION,
                                         0, 0, Wake, &Proof};
  switchyard_fex_process* Process {};
  switchyard_fex_thread* Thread {};
  Require(switchyard_fex_process_create(&Config, &Process));
  Require(switchyard_fex_thread_create_with_domain(Process, &Domain, &Thread, &Proof.Cell));
  auto& Frame = *Thread->CoreThread->CurrentFrame;
  auto& State = Frame.State;
  const bool DetectedSplit = Process->SplitVectorState;
  alignas(4) uint32_t Doorbell {};
  switchyard_fex_execution Execution {};
  Execution.size = sizeof(Execution);
  Execution.version = SWITCHYARD_FEX_EXECUTION_VERSION;
  Execution.suspend_doorbell = reinterpret_cast<uintptr_t>(&Doorbell);
  std::array<unsigned char, 424> Output;
  xtajit64_x64_context Expected;
  unsigned Cases {}, Negatives {};
  constexpr uint64_t RIP = 0x10000;
  for (const bool Split : {false, true}) {
    Process->SplitVectorState = Split;
    for (unsigned Sample = 0; Sample < 32; ++Sample) {
      switchyard_fex_x64_state Seed {};
      Seed.size = sizeof(Seed);
      Seed.version = SWITCHYARD_FEX_STATE_VERSION;
      Seed.flags = SWITCHYARD_FEX_STATE_YMM_HIGH_VALID;
      Seed.rip = RIP;
      Seed.rflags = 0x202u | (Sample & 1u ? 0x845u : 0x490u);
      Seed.mxcsr = 0x1f80u | ((Sample & 3u) << 13) | (Sample & 4u ? 0x40u : 0);
      Seed.fcw = 0x27f;
      Seed.abridged_ftw = 0xff;
      Seed.segment[1] = 0x30;
      Seed.segment_base[5] = 0xffe000 + Sample * 4096u;
      std::memset(Seed.x87, static_cast<int>(Sample + 1), sizeof(Seed.x87));
      for (unsigned Index = 0; Index < 16; ++Index) {
        Seed.gpr[Index] = 0x73124ac900000000ULL + Sample * 16u + Index;
        Seed.xmm[Index] = {0x5a71730000000000ULL + Sample + Index,
                           0xf124756200000000ULL + Sample + Index};
        Seed.ymm_high[Index] = {0x34adbf9000000000ULL + Sample + Index,
                                0x2142738100000000ULL + Sample + Index};
      }
      Require(switchyard_fex_thread_import_state(Thread, &Seed));
      for (unsigned Kind = 0; Kind < 8; ++Kind) {
        const bool Publishes = Kind != 1 && Kind != 7;
        switchyard_fex_x64_state Full {.size = sizeof(Full), .version = SWITCHYARD_FEX_STATE_VERSION};
        Require(switchyard_fex_thread_export_state(Thread, &Full));
        export_fex_state(&Expected, &Full); // Independent Wine full-state conversion.
        switchyard_fex_x64_state RoundTrip;
        import_fex_state(&RoundTrip, &Expected, Full.segment_base[5]);
        Check(std::memcmp(RoundTrip.gpr, Full.gpr, sizeof(Full.gpr)) == 0 &&
              RoundTrip.rip == Full.rip && RoundTrip.rflags == Full.rflags &&
              RoundTrip.mxcsr == Full.mxcsr &&
              std::memcmp(RoundTrip.xmm, Full.xmm, sizeof(Full.xmm)) == 0);
        std::array<unsigned char, sizeof(State)> Before;
        std::memcpy(Before.data(), &State, sizeof(State));
        Output.fill(0xa5);
        const size_t Offset = Sample & 15u;
        auto Out = Window(Output.data() + Offset);
        auto Stop = StopValue();
        uint64_t Generation {};
        Execution.expected_hlt_rip = Kind == 3 ? RIP : 0;
        Require(switchyard_fex_thread_prepare_execution(Thread, &Execution, &Generation));
        switchyard_fex_stop_reason Reason {};
        switchyard_fex_result Result = SWITCHYARD_FEX_OK;
        using External = FEXCore::Core::ExternalStopReason;
        switch (Kind) {
        case 0: Frame.ExternalStop = External::Transition; Reason = SWITCHYARD_FEX_STOP_EC_TRANSITION; break;
        case 1: Frame.ExternalStop = External::Syscall; Reason = SWITCHYARD_FEX_STOP_SYSCALL; break;
        case 2: Frame.ExternalStop = External::Suspend; Reason = SWITCHYARD_FEX_STOP_SUSPEND; break;
        case 3:
          Frame.SynchronousFaultData.Signal = FEXCore::Core::FAULT_SIGSEGV;
          Frame.SynchronousFaultData.TrapNo = FEXCore::X86State::X86_TRAPNO_GP;
          Reason = SWITCHYARD_FEX_STOP_HLT; break;
        case 4:
          Frame.SynchronousFaultData.Signal = FEXCore::Core::FAULT_SIGILL;
          Frame.SynchronousFaultData.TrapNo = FEXCore::X86State::X86_TRAPNO_UD;
          Reason = SWITCHYARD_FEX_STOP_INVALID_INSTRUCTION; break;
        case 5:
          Frame.SynchronousFaultData.Signal = FEXCore::Core::FAULT_SIGTRAP;
          Frame.SynchronousFaultData.TrapNo = FEXCore::X86State::X86_TRAPNO_DB;
          Reason = SWITCHYARD_FEX_STOP_SINGLE_STEP; break;
        case 6: Reason = SWITCHYARD_FEX_STOP_GUEST_FAULT; Result = SWITCHYARD_FEX_ERROR_GUEST_FAULT; break;
        case 7:
          Frame.ExternalStop = External::InvalidCustomABI;
          Reason = SWITCHYARD_FEX_STOP_INTERNAL; Result = SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT; break;
        }
        Require(ValidateCompletionWindow(Thread, &Stop, &Out));
        std::array<unsigned char, 408> Unchanged;
        Unchanged.fill(0xa5);
        Proof.Output = Output.data() + Offset;
        Proof.Expected = Publishes ? reinterpret_cast<const unsigned char*>(&Expected) : Unchanged.data();
        { std::lock_guard Lock(Proof.Mutex); switchyard_fex_admission_close(Proof.Cell); }
        Check(CompleteExecution({Thread, Generation}, &Stop, &Out) == Result);
        Check(Stop.reason == Reason && Stop.rip == RIP && Proof.Wakes == Cases + 1);
        Check(std::memcmp(Before.data(), &State, sizeof(State)) == 0);
        for (size_t Index = 0; Index < Offset; ++Index) Check(Output[Index] == 0xa5);
        for (size_t Index = Offset + 408; Index < Output.size(); ++Index) Check(Output[Index] == 0xa5);
        switchyard_fex_admission_reopen(Proof.Cell);
        Check(CompleteExecution({Thread, Generation}, &Stop, &Out) == SWITCHYARD_FEX_ERROR_BUSY);
        ++Cases;
      }
    }
  }
  // Invalid output descriptors must not execute, consume, or publish, so the
  // owner can retry the same capability with a valid stopped-state completion.
  Process->SplitVectorState = DetectedSplit;
  Output.fill(0xa5);
  auto Out = Window(Output.data());
  auto Stop = StopValue();
  uint64_t Generation {};
  Require(switchyard_fex_thread_prepare_execution(Thread, &Execution, &Generation));
  const auto Token = switchyard_fex_admission_load(Proof.Cell);
  for (unsigned Kind = 0; Kind < 15; ++Kind) {
    auto Bad = Out;
    auto BadStop = Stop;
    auto* StopPointer = &BadStop;
    const auto* WindowPointer = &Bad;
    auto BadGeneration = Generation;
    switch (Kind) {
    case 0: --Bad.size; break;
    case 1: ++Bad.version; break;
    case 2: Bad.gs_base = 1; break;
    case 3: ++Bad.data_size; break;
    case 4: Bad.data = UINT64_MAX - 406; break;
    case 5: Bad.data = 0; break;
    case 6: Bad.data = reinterpret_cast<uintptr_t>(&BadStop); break;
    case 7: Bad.data = reinterpret_cast<uintptr_t>(&Bad); break;
    case 8: Bad.data = reinterpret_cast<uintptr_t>(Thread); break;
    case 9: Bad.data = reinterpret_cast<uintptr_t>(&Frame); break;
    case 10: --BadStop.size; break;
    case 11: ++BadStop.version; break;
    case 12: StopPointer = nullptr; break;
    case 13: WindowPointer = nullptr; break;
    case 14: --BadGeneration; break;
    }
    const auto ExpectedResult = Kind == 1 || Kind == 11 ? SWITCHYARD_FEX_ERROR_ABI_MISMATCH :
      Kind == 14 ? SWITCHYARD_FEX_ERROR_BUSY : SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
    const auto StopBefore = BadStop;
    Check(switchyard_fex_experiment_execute_export_window(Thread, BadGeneration, StopPointer, WindowPointer) == ExpectedResult);
    Check(switchyard_fex_admission_load(Proof.Cell) == Token && std::memcmp(&StopBefore, &BadStop, sizeof(BadStop)) == 0);
    for (const auto Byte : Output) Check(Byte == 0xa5);
    ++Negatives;
  }
  Frame.ExternalStop = FEXCore::Core::ExternalStopReason::Transition;
  Check(CompleteExecution({Thread, Generation}, &Stop, &Out) == SWITCHYARD_FEX_OK);
  switchyard_fex_admission_close(Proof.Cell);
  Require(switchyard_fex_thread_destroy(Thread));
  Require(switchyard_fex_process_destroy(Process));
  std::printf("completion_window_state result=pass cases=%u core_bytes=%zu negatives=%u wakes=%u\n",
              Cases, sizeof(State), Negatives, Proof.Wakes);
}
