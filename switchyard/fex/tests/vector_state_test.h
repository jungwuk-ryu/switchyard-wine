// SPDX-License-Identifier: MIT

// Included by the white-box ABI test. No execution or other threads exist while
// toggling the normally immutable capability. Compare against the real core
// helpers, including every untouched CPUState byte and C ABI reserved byte.
void RunVectorStateTests(switchyard_fex_process* Process) {
  const auto Features = DetectHostFeatures();
  Check(Features && Features->SupportsAVX && !Features->SupportsSVE256);
  Check(Process->SplitVectorState);
  switchyard_fex_thread* Thread {};
  Require(switchyard_fex_thread_create(Process, &Thread));
  auto& State = Thread->CoreThread->CurrentFrame->State;
  std::array<unsigned char, sizeof(State)> Initial {}, Reference {};
  uint64_t Random = 0x12b75ca4809fed36ULL;
  auto Next = [&]() {
    Random = Random * 6364136223846793005ULL + 1442695040888963407ULL;
    return Random;
  };
  struct alignas(16) GuardedState {
    uint64_t Before;
    switchyard_fex_x64_state Value;
    uint64_t After;
  };
  static_assert(offsetof(GuardedState, Value) == 8); // Not uint128_t aligned.
  constexpr uint64_t Canary = 0xb798cf20653dea41ULL;

  for (unsigned Iteration = 0; Iteration < 130; ++Iteration) {
    GuardedState Input {Canary, {}, Canary};
    Input.Value.size = sizeof(Input.Value);
    Input.Value.version = SWITCHYARD_FEX_STATE_VERSION;
    Input.Value.rip = Next();
    Input.Value.rflags = 0x202 | (Next() & 0xcd5);
    Input.Value.mxcsr = static_cast<uint32_t>(Next() & ValidMXCSRMask);
    Input.Value.fcw = static_cast<uint16_t>(Next());
    Input.Value.abridged_ftw = static_cast<uint8_t>(Next());
    for (auto& Value : Input.Value.gpr) Value = Next();
    for (unsigned i = 0; i < 6; ++i) {
      Input.Value.segment[i] = static_cast<uint16_t>(Next());
      Input.Value.segment_base[i] = i < 4 ? static_cast<uint32_t>(Next()) : Next();
    }
    for (auto& Value : Input.Value.x87) Value = {Next(), Next()};
    for (unsigned i = 0; i < 16; ++i) {
      Input.Value.xmm[i] = {Next(), Next()};
      Input.Value.ymm_high[i] = {Next(), Next()};
      if (Iteration < 2) {
        const uint64_t Bits = Iteration ? UINT64_MAX : 0;
        Input.Value.xmm[i] = Input.Value.ymm_high[i] = {Bits, Bits};
      }
      State.xmm.sse.data[i][0] = Next();
      State.xmm.sse.data[i][1] = Next();
      State.xmm.sse.pad[i][0] = Next();
      State.xmm.sse.pad[i][1] = Next();
      State.avx_high[i][0] = Next();
      State.avx_high[i][1] = Next();
    }
    std::memcpy(Initial.data(), &State, sizeof(State));
    for (const bool HighValid : {false, true}) {
      Input.Value.flags = HighValid ? SWITCHYARD_FEX_STATE_YMM_HIGH_VALID : 0;
      GuardedState Outputs[2];
      for (const unsigned Direct : {0U, 1U}) {
        Process->SplitVectorState = Direct != 0;
        std::memcpy(&State, Initial.data(), sizeof(State));
        Require(switchyard_fex_thread_import_state(Thread, &Input.Value));
        if (!Direct) std::memcpy(Reference.data(), &State, sizeof(State));
        else Check(std::memcmp(Reference.data(), &State, sizeof(State)) == 0);
        if (!HighValid)
          Check(std::memcmp(State.avx_high, Initial.data() + offsetof(FEXCore::Core::CPUState, avx_high),
                            sizeof(State.avx_high)) == 0);
        auto& Output = Outputs[Direct];
        // Different poisons make omitted bytes fail the whole-output oracle.
        std::memset(&Output, Direct ? 0x5a : 0xa5, sizeof(Output));
        Output.Before = Output.After = Canary;
        Output.Value.size = sizeof(Output.Value);
        Output.Value.version = SWITCHYARD_FEX_STATE_VERSION;
        Require(switchyard_fex_thread_export_state(Thread, &Output.Value));
        Check(Output.Before == Canary && Output.After == Canary);
        Check(Output.Value.reserved == 0 && Output.Value.reserved_fp == 0 &&
              Output.Value.reserved_segment == 0);
        Check(std::memcmp(Output.Value.xmm, Input.Value.xmm, sizeof(Input.Value.xmm)) == 0);
        if (HighValid)
          Check(std::memcmp(Output.Value.ymm_high, Input.Value.ymm_high, sizeof(Input.Value.ymm_high)) == 0);
      }
      Check(std::memcmp(&Outputs[0].Value, &Outputs[1].Value, sizeof(Outputs[0].Value)) == 0);
      Check(Input.Before == Canary && Input.After == Canary);
    }
  }
  // Validation and exclusive ownership precede either vector path.
  for (const bool Direct : {false, true}) {
    Process->SplitVectorState = Direct;
    switchyard_fex_x64_state Invalid {};
    Invalid.size = sizeof(Invalid);
    Invalid.version = SWITCHYARD_FEX_STATE_VERSION;
    std::memcpy(Initial.data(), &State, sizeof(State));
    Invalid.flags = ~KnownStateFlags;
    Check(switchyard_fex_thread_import_state(Thread, &Invalid) == SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
    Invalid.flags = 0;
    Invalid.segment_base[3] = uint64_t {1} << 32;
    Check(switchyard_fex_thread_import_state(Thread, &Invalid) == SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
    Invalid.segment_base[3] = 0;
    GuardedState Rejected;
    std::memset(&Rejected, 0xa5, sizeof(Rejected));
    Rejected.Before = Rejected.After = Canary;
    Rejected.Value.size = sizeof(Rejected.Value) - 1;
    Rejected.Value.version = SWITCHYARD_FEX_STATE_VERSION;
    GuardedState BeforeRejection;
    std::memcpy(&BeforeRejection, &Rejected, sizeof(Rejected));
    Check(switchyard_fex_thread_export_state(Thread, &Rejected.Value) == SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
    Check(std::memcmp(&Rejected, &BeforeRejection, sizeof(Rejected)) == 0);
    Rejected.Value.size = sizeof(Rejected.Value);
    Rejected.Value.version = SWITCHYARD_FEX_STATE_VERSION + 1;
    std::memcpy(&BeforeRejection, &Rejected, sizeof(Rejected));
    Check(switchyard_fex_thread_export_state(Thread, &Rejected.Value) == SWITCHYARD_FEX_ERROR_ABI_MISMATCH);
    Check(std::memcmp(&Rejected, &BeforeRejection, sizeof(Rejected)) == 0);
    Rejected.Value.version = SWITCHYARD_FEX_STATE_VERSION;
    std::memcpy(&BeforeRejection, &Rejected, sizeof(Rejected));
    uint64_t Token {};
    Check(switchyard_fex_admission_acquire(&Thread->Admission, false, &Token) == 1);
    Check(switchyard_fex_thread_import_state(Thread, &Invalid) == SWITCHYARD_FEX_ERROR_BUSY);
    Check(switchyard_fex_thread_export_state(Thread, &Invalid) == SWITCHYARD_FEX_ERROR_BUSY);
    Check(switchyard_fex_thread_export_state(Thread, &Rejected.Value) == SWITCHYARD_FEX_ERROR_BUSY);
    Check(std::memcmp(&Rejected, &BeforeRejection, sizeof(Rejected)) == 0);
    Check(switchyard_fex_admission_release(&Thread->Admission, Token));
    Check(std::memcmp(Initial.data(), &State, sizeof(State)) == 0);
  }
  Process->SplitVectorState = true;
  Require(switchyard_fex_thread_destroy(Thread));
  std::puts("Split-vector state: 260 whole-byte core-helper cases, distinct output poisons, reserved bytes, canaries and rejection paths passed");
}
