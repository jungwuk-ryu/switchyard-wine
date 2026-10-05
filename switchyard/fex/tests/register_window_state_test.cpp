// SPDX-License-Identifier: MIT
// Actual-core byte differential against the independent full-state Wine contract.
#include "../src/switchyard_fex.cpp"
#include "register_window_reference.h"
#include <array>
#include <source_location>

static void Check(bool Value, const std::source_location Site = std::source_location::current()) {
  if (Value) return;
  std::fprintf(stderr, "window whole-state line %u\n", Site.line());
  std::_Exit(97);
}
static void Require(switchyard_fex_result Result) { Check(Result == SWITCHYARD_FEX_OK); }
static switchyard_fex_register_window Window(void* Data, uint64_t GS = 0) {
  return {sizeof(switchyard_fex_register_window), SWITCHYARD_FEX_REGISTER_WINDOW_VERSION,
          0, 0, reinterpret_cast<uintptr_t>(Data), sizeof(xtajit64_x64_context), GS};
}
static_assert(sizeof(xtajit64_x64_context) == SWITCHYARD_FEX_REGISTER_WINDOW_DATA_SIZE_V1);
static_assert(offsetof(xtajit64_x64_context, xmm) == SWITCHYARD_FEX_WINDOW_XMM);
struct alignas(16) Guarded {
  uint64_t Before;
  xtajit64_x64_context Data;
  uint64_t After;
};
constexpr uint64_t Canary = 0xcaf817563092b4deULL;

int main() {
  switchyard_fex_config Config {};
  Config.size = sizeof(Config);
  Config.abi_version = SWITCHYARD_FEX_ABI_VERSION;
  Config.flags = SWITCHYARD_FEX_CONFIG_MULTIBLOCK;
  switchyard_fex_process* Process {};
  switchyard_fex_thread* Thread {};
  Require(switchyard_fex_process_create(&Config, &Process));
  Require(switchyard_fex_thread_create(Process, &Thread));
  auto& State = Thread->CoreThread->CurrentFrame->State;
  std::array<unsigned char, sizeof(State)> Initial {}, Reference {}, Saved {};
  std::memcpy(Saved.data(), &State, sizeof(State));
  const bool DetectedSplit = Process->SplitVectorState;
  uint64_t Random = 0x17c38564b902eda1ULL;
  auto Next = [&]() { Random = Random * 6364136223846793005ULL + 1442695040888963407ULL; return Random; };
  unsigned Cases = 0;
  for (unsigned Iteration = 0; Iteration < 132; ++Iteration) {
    Guarded Input {Canary, {}, Canary};
    auto* Bytes = reinterpret_cast<unsigned char*>(&Input.Data);
    for (size_t Index = 0; Index < sizeof(Input.Data); ++Index)
      Bytes[Index] = Iteration < 2 ? static_cast<unsigned char>(Iteration ? 255 : 0) : static_cast<unsigned char>(Next() >> 56);
    Input.Data.eflags &= UINT32_MAX;
    Input.Data.mxcsr &= ValidMXCSRMask;
    const auto InputBefore = Input;
    const uint64_t GS = Iteration == 0 ? 0 : Iteration == 1 ? UINT64_MAX : Next();
    std::memcpy(&State, Saved.data(), sizeof(State));
    for (auto& Lane : State.avx_high) { Lane[0] = Next(); Lane[1] = Next(); }
    for (auto& Lane : State.xmm.sse.pad) { Lane[0] = Next(); Lane[1] = Next(); }
    for (auto& Lane : State.mm) { Lane[0] = Next(); Lane[1] = Next(); }
    std::memcpy(Initial.data(), &State, sizeof(State));
    for (const bool Split : {false, true}) {
      Process->SplitVectorState = Split;
      std::memcpy(&State, Initial.data(), sizeof(State));
      switchyard_fex_x64_state Full;
      import_fex_state(&Full, &Input.Data, GS);
      Require(switchyard_fex_thread_import_state(Thread, &Full));
      std::memcpy(Reference.data(), &State, sizeof(State));
      Guarded Before {Canary, {}, Canary}, After {Canary, {}, Canary};
      std::memset(&Before.Data, 0xa5, sizeof(Before.Data));
      std::memset(&After.Data, 0x5a, sizeof(After.Data));
      Full.size = sizeof(Full);
      Full.version = SWITCHYARD_FEX_STATE_VERSION;
      Require(switchyard_fex_thread_export_state(Thread, &Full));
      export_fex_state(&Before.Data, &Full);
      std::memcpy(&State, Initial.data(), sizeof(State));
      const auto In = Window(&Input.Data, GS), Out = Window(&After.Data);
      Require(switchyard_fex_thread_import_register_window(Thread, &In));
      Check(std::memcmp(Reference.data(), &State, sizeof(State)) == 0);
      Require(switchyard_fex_thread_export_register_window(Thread, &Out));
      Check(std::memcmp(&Before.Data, &After.Data, sizeof(Before.Data)) == 0);
      Check(Before.Before == Canary && Before.After == Canary && After.Before == Canary && After.After == Canary);
      Check(std::memcmp(&Input, &InputBefore, sizeof(Input)) == 0);
      Check(std::memcmp(State.avx_high, Initial.data() + offsetof(FEXCore::Core::CPUState, avx_high), sizeof(State.avx_high)) == 0);
      ++Cases;
    }
  }
  for (const bool Split : {false, true}) {
    Process->SplitVectorState = Split;
    xtajit64_x64_context Input {}, Output, Before;
    std::memset(&Output, 0xa5, sizeof(Output));
    Before = Output;
    const auto In = Window(&Input), Out = Window(&Output);
    Thread->StateValid = false;
    Check(switchyard_fex_thread_export_register_window(Thread, &Out) == SWITCHYARD_FEX_ERROR_BUSY);
    Check(std::memcmp(&Output, &Before, sizeof(Output)) == 0);
    std::memcpy(Initial.data(), &State, sizeof(State));
    Input.eflags = uint64_t {1} << 32;
    Check(switchyard_fex_thread_import_register_window(Thread, &In) == SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
    Input.eflags = 0;
    Input.mxcsr = 0x10000;
    Check(switchyard_fex_thread_import_register_window(Thread, &In) == SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
    Input.mxcsr = 0;
    uint64_t Token {};
    Check(switchyard_fex_admission_acquire(&Thread->Admission, false, &Token) == 1);
    Check(switchyard_fex_thread_import_register_window(Thread, &In) == SWITCHYARD_FEX_ERROR_BUSY);
    Check(switchyard_fex_thread_export_register_window(Thread, &Out) == SWITCHYARD_FEX_ERROR_BUSY);
    Check(switchyard_fex_admission_release(&Thread->Admission, Token));
    switchyard_fex_admission_close(&Thread->Admission);
    Check(switchyard_fex_thread_import_register_window(Thread, &In) == SWITCHYARD_FEX_ERROR_BUSY);
    Check(switchyard_fex_thread_export_register_window(Thread, &Out) == SWITCHYARD_FEX_ERROR_BUSY);
    switchyard_fex_admission_reopen(&Thread->Admission);
    for (const auto Pointer : {reinterpret_cast<uintptr_t>(Thread), reinterpret_cast<uintptr_t>(Process),
                              reinterpret_cast<uintptr_t>(Thread->CoreThread), reinterpret_cast<uintptr_t>(&State)}) {
      auto Alias = In;
      Alias.data = Pointer;
      Check(switchyard_fex_thread_import_register_window(Thread, &Alias) == SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
      Check(switchyard_fex_thread_export_register_window(Thread, &Alias) == SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
    }
    Check(std::memcmp(Initial.data(), &State, sizeof(State)) == 0);
    Check(std::memcmp(&Output, &Before, sizeof(Output)) == 0);
  }
  std::memcpy(&State, Saved.data(), sizeof(State));
  Process->SplitVectorState = DetectedSplit;
  Require(switchyard_fex_thread_destroy(Thread));
  Require(switchyard_fex_process_destroy(Process));
  std::printf("register_window_state result=pass cases=%u core_bytes=%zu data_bytes=408\n", Cases, sizeof(State));
}
