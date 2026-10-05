// SPDX-License-Identifier: MIT
// Included by the white-box Darwin ABI test, inside its anonymous namespace.
// This is an embedding fixture, not a PE thunk or a Windows continuation model.

struct DetachedObservation {
  uint64_t EntrySP {}, ReturnSP {}, EntryX18 {}, ReturnX18 {}, CustomAfter {};
};
struct DetachedInvocation {
  uintptr_t Entry;
  FEXCore::Core::CpuStateFrame* Frame;
  uintptr_t EnterMode, LeaveMode, ExpectedX18, StackTop;
  DetachedObservation* Observation;
  bool (*QueryMode)();
};
static_assert(sizeof(DetachedInvocation) == 64 && offsetof(DetachedInvocation, Frame) == 8 &&
              offsetof(DetachedInvocation, QueryMode) == 56 && sizeof(DetachedObservation) == 40);

// Save the ordinary caller only on its host stack, then use the SAME empty
// private stack for every invocation. Only generated code and this resource-free
// trampoline are live there. It has fully returned before CompleteExecution or
// any simulated native interval. Preserve the host's FP state as well as its ABI.
__attribute__((naked)) void InvokeDetached(const DetachedInvocation*) {
  __asm__("sub sp, sp, #176\n\t"
          "stp x19, x20, [sp]\n\tstp x21, x22, [sp, #16]\n\t"
          "stp x23, x24, [sp, #32]\n\tstp x25, x26, [sp, #48]\n\t"
          "stp x27, x28, [sp, #64]\n\tstp x29, x30, [sp, #80]\n\t"
          "stp d8, d9, [sp, #96]\n\tstp d10, d11, [sp, #112]\n\t"
          "stp d12, d13, [sp, #128]\n\tstp d14, d15, [sp, #144]\n\t"
          "mrs x1, fpcr\n\tmrs x2, fpsr\n\tstp x1, x2, [sp, #160]\n\t"
          "mov x19, x0\n\tmov x21, sp\n\t"
          "ldr x20, [x19, #48]\n\tldr x22, [x19, #40]\n\t"
          "ldr x28, [x19, #8]\n\tmov sp, x22\n\t"
          "ldr x16, [x19, #16]\n\tblr x16\n\t"
          "ldr x18, [x19, #32]\n\t"
          "str x22, [x20]\n\tstr x18, [x20, #16]\n\t"
          "brk #0x7e0\n\t"
          "ldr x0, [x19, #8]\n\tmov w1, #0\n\t"
          "ldr x16, [x19]\n\tblr x16\n\t"
          "brk #0x7e0\n\t"
          "mov x9, sp\n\tstr x9, [x20, #8]\n\tstr x18, [x20, #24]\n\t"
          "ldr x16, [x19, #56]\n\tblr x16\n\tstr x0, [x20, #32]\n\t"
          "cbz w0, 1f\n\tldr x16, [x19, #24]\n\tblr x16\n\t"
          "1: mov sp, x21\n\t"
          "ldp x1, x2, [sp, #160]\n\tmsr fpcr, x1\n\tmsr fpsr, x2\n\t"
          "ldp d8, d9, [sp, #96]\n\tldp d10, d11, [sp, #112]\n\t"
          "ldp d12, d13, [sp, #128]\n\tldp d14, d15, [sp, #144]\n\t"
          "ldp x19, x20, [sp]\n\tldp x21, x22, [sp, #16]\n\t"
          "ldp x23, x24, [sp, #32]\n\tldp x25, x26, [sp, #48]\n\t"
          "ldp x27, x28, [sp, #64]\n\tldp x29, x30, [sp, #80]\n\t"
          "add sp, sp, #176\n\tret");
}

sigjmp_buf DetachedFaultJump;
switchyard_fex_arm64_host_context DetachedFaultContext;
uint64_t DetachedFaultAddress;
volatile sig_atomic_t DetachedFaultArmed, DetachedFaultSignal;

void DetachedFaultPayload(int Signal, siginfo_t* Info, void* Opaque) {
  if (!DetachedFaultArmed || os_custom_x18_abi_enabled()) _exit(97);
  const auto* Context = static_cast<const ucontext_t*>(Opaque);
  auto& Host = DetachedFaultContext;
  Host = {};
  Host.size = sizeof(Host);
  Host.version = SWITCHYARD_FEX_ARM64_HOST_CONTEXT_VERSION;
  for (size_t i = 0; i < 29; ++i) Host.gpr[i] = Context->uc_mcontext->__ss.__x[i];
  Host.gpr[29] = Context->uc_mcontext->__ss.__fp;
  Host.gpr[30] = Context->uc_mcontext->__ss.__lr;
  for (size_t i = 0; i < 32; ++i)
    std::memcpy(&Host.vector[i], &Context->uc_mcontext->__ns.__v[i], sizeof(Host.vector[i]));
  Host.pc = Context->uc_mcontext->__ss.__pc;
  Host.pstate = Context->uc_mcontext->__ss.__cpsr;
  Host.fpcr = Context->uc_mcontext->__ns.__fpcr;
  Host.fpsr = Context->uc_mcontext->__ns.__fpsr;
  DetachedFaultAddress = reinterpret_cast<uintptr_t>(Info->si_addr);
  DetachedFaultSignal = Signal;
  DetachedFaultArmed = 0;
  // Leave no C++ frame/lock behind. This C-ABI fault-consumption fixture has
  // no Wine TEB/syscall frame; it is not a Wine exception-dispatch proof.
  siglongjmp(DetachedFaultJump, 1);
}

void DetachedFaultHandler(int Signal, siginfo_t* Info, void* Opaque) {
  // The real Wine wrapper intentionally fails closed for memory faults in the
  // standalone shim (no TEB for lost-x18 recovery). Use the SDK mode boundary
  // for this adapter-only nonlocal test; do not fake Wine recovery as success.
  if (os_custom_x18_abi_enabled()) os_set_custom_x18_abi_enabled(false);
  DetachedFaultPayload(Signal, Info, Opaque);
}

void RunPublicDispatchTests() {
  switchyard_fex_config Config {};
  Config.size = sizeof(Config);
  Config.abi_version = SWITCHYARD_FEX_ABI_VERSION;
  Config.flags = SWITCHYARD_FEX_CONFIG_MULTIBLOCK | SWITCHYARD_FEX_CONFIG_CUSTOM_DISPATCH;
  switchyard_fex_process* Process {};
  Require(switchyard_fex_process_create(&Config, &Process));
  alignas(16) uint8_t Code[] = {0x48, 0xb8, 17, 0, 0, 0, 0, 0, 0, 0, 0xf4};
  ExecutableFixture Current {Code, sizeof(Code)};
  Require(switchyard_fex_process_set_executable_range_query(Process, QueryCode, &Current));
  switchyard_fex_thread* Thread {};
  Require(switchyard_fex_thread_create(Process, &Thread));
  const auto Page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  constexpr size_t StackSize = 256 * 1024; // Wine's current private control stack.
  Check(std::has_single_bit(Page) && Page <= StackSize && !(StackSize % Page));
  auto* Stack = static_cast<uint8_t*>(mmap(nullptr, StackSize + 2 * Page,
      PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0));
  Check(Stack != MAP_FAILED && !mprotect(Stack + Page, StackSize, PROT_READ | PROT_WRITE));
  alignas(16) uint8_t GuestStack[4096] {};
  switchyard_fex_x64_state Input {};
  Input.size = sizeof(Input);
  Input.version = SWITCHYARD_FEX_STATE_VERSION;
  Input.rip = reinterpret_cast<uintptr_t>(Code);
  Input.gpr[4] = reinterpret_cast<uintptr_t>(GuestStack + sizeof(GuestStack));
  Input.rflags = 0x202;
  Input.mxcsr = 0x1f80;
  Input.fcw = 0x37f;
  Input.segment[1] = FEXCore::Core::CPUState::DEFAULT_USER_CS << 3;
  std::atomic_uint Doorbell {};
  switchyard_fex_execution Execution {sizeof(Execution), SWITCHYARD_FEX_EXECUTION_VERSION,
      0, 0, Input.rip + sizeof(Code) - 1, reinterpret_cast<uintptr_t>(&Doorbell)};
  switchyard_fex_dispatch Dispatch {sizeof(Dispatch), SWITCHYARD_FEX_DISPATCH_VERSION, 0, 0, 0, 0, 0};
  switchyard_fex_stop Stop {};
  Stop.size = sizeof(Stop);
  Stop.version = SWITCHYARD_FEX_STOP_VERSION;
  Require(switchyard_fex_thread_import_state(Thread, &Input));
  Check(switchyard_fex_thread_prepare_dispatch(Thread, &Execution, 0, &Dispatch) ==
        SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT && !Dispatch.generation);
  Dispatch.version++;
  Check(switchyard_fex_thread_prepare_dispatch(Thread, &Execution, CustomValue, &Dispatch) ==
        SWITCHYARD_FEX_ERROR_ABI_MISMATCH && !Dispatch.generation);
  Dispatch.version--;
  uint64_t PreviousGeneration {};
  for (unsigned Iteration = 0; Iteration < 3; ++Iteration) {
    Require(switchyard_fex_thread_import_state(Thread, &Input));
    Require(switchyard_fex_thread_prepare_dispatch(Thread, &Execution, CustomValue, &Dispatch));
    Check(Dispatch.entry && Dispatch.frame && Dispatch.generation > PreviousGeneration);
    Check(switchyard_fex_thread_complete_dispatch(Thread, PreviousGeneration, &Stop) == SWITCHYARD_FEX_ERROR_BUSY);
    Check(switchyard_fex_process_invalidate_code(Process, Input.rip, sizeof(Code)) == SWITCHYARD_FEX_ERROR_BUSY);
    Check(switchyard_fex_thread_prepare_dispatch(Thread, &Execution, CustomValue, &Dispatch) == SWITCHYARD_FEX_ERROR_BUSY);
    std::thread Foreign([&]() {
      switchyard_fex_stop Other {};
      Other.size = sizeof(Other);
      Other.version = SWITCHYARD_FEX_STOP_VERSION;
      Check(switchyard_fex_thread_complete_dispatch(Thread, Dispatch.generation, &Other) == SWITCHYARD_FEX_ERROR_BUSY);
    });
    Foreign.join();
    auto* Frame = Thread->CoreThread->CurrentFrame;
    DetachedObservation Observation {};
    DetachedInvocation Invocation {Dispatch.entry, reinterpret_cast<FEXCore::Core::CpuStateFrame*>(Dispatch.frame),
        Frame->EnterCustomABI, Frame->LeaveCustomABI, CustomValue,
        reinterpret_cast<uintptr_t>(Stack + Page + StackSize), &Observation, os_custom_x18_abi_enabled};
    InvokeDetached(&Invocation);
    Check(Observation.CustomAfter == 1 && Observation.ReturnX18 == CustomValue &&
          Observation.ReturnSP == Invocation.StackTop && !os_custom_x18_abi_enabled());
    Stop.version++;
    Check(switchyard_fex_thread_complete_dispatch(Thread, Dispatch.generation, &Stop) == SWITCHYARD_FEX_ERROR_ABI_MISMATCH);
    Stop.version--;
    Require(switchyard_fex_thread_complete_dispatch(Thread, Dispatch.generation, &Stop));
    Check(Stop.reason == (Iteration == 2 ? SWITCHYARD_FEX_STOP_SUSPEND : SWITCHYARD_FEX_STOP_HLT));
    Check(switchyard_fex_thread_complete_dispatch(Thread, Dispatch.generation, &Stop) == SWITCHYARD_FEX_ERROR_BUSY);
    switchyard_fex_x64_state Output {};
    Output.size = sizeof(Output);
    Output.version = SWITCHYARD_FEX_STATE_VERSION;
    Require(switchyard_fex_thread_export_state(Thread, &Output));
    Check(Output.gpr[0] == (Iteration == 2 ? 0U : Code[2]));
    PreviousGeneration = Dispatch.generation;
    // Native interval: replacement at the same address, same private stack.
    Code[2] = 34;
    Require(switchyard_fex_process_invalidate_code(Process, Input.rip, sizeof(Code)));
    if (Iteration == 1) Doorbell.store(1);
  }
  Require(switchyard_fex_thread_destroy(Thread));
  Require(switchyard_fex_process_destroy(Process));
  Check(!munmap(Stack, StackSize + 2 * Page));
  std::puts("Public detached dispatch: configuration, tokens, mutation exclusion and same-stack reuse passed");
}

void RunDetachedTests(switchyard_fex_thread* Thread, ExecutableFixture& Current, bool OrdinaryControl) {
  auto* const Process = Thread->Process;
  auto* const Frame = Thread->CoreThread->CurrentFrame;
  const auto PageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  Check(std::has_single_bit(PageSize));
  constexpr size_t UsableStack = 1024 * 1024;
  Check(PageSize <= UsableStack && !(UsableStack % PageSize));
  const auto AllocationSize = UsableStack + PageSize * 2;
  auto* const Allocation = static_cast<uint8_t*>(mmap(nullptr, AllocationSize, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0));
  Check(Allocation != MAP_FAILED);
  Check(mprotect(Allocation + PageSize, UsableStack, PROT_READ | PROT_WRITE) == 0);
  const auto StackTop = reinterpret_cast<uintptr_t>(Allocation + PageSize + UsableStack);
  alignas(16) uint8_t GuestStacks[2][4096] {};
  alignas(16) uint8_t Code[] = {0x48, 0xb8, 0x11, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe3, // MOV RAX,17; JMP RBX
                              0x90, 0xf4, // nested NOP; HLT
                              0x48, 0x8b, 0x01, 0xf4, // MOV RAX,[RCX]; HLT
                              0x0f, 0xa2, 0xf4}; // real Mach-O CPUID callback; HLT
  Current = {Code, sizeof(Code)};
  switchyard_fex_x64_state Input {}, Output {};
  Input.size = sizeof(Input);
  Input.version = SWITCHYARD_FEX_STATE_VERSION;
  Input.flags = SWITCHYARD_FEX_STATE_YMM_HIGH_VALID;
  Input.rip = reinterpret_cast<uintptr_t>(Code);
  Input.rflags = 0x246;
  Input.mxcsr = 0x1f80;
  Input.fcw = 0x37f;
  Input.segment[1] = FEXCore::Core::CPUState::DEFAULT_USER_CS << 3;
  for (size_t i = 0; i < 16; ++i) {
    Input.gpr[i] = 0x8000 + i;
    Input.xmm[i] = {0x12340000 + i, 0x56780000 + i};
    Input.ymm_high[i] = {0x98760000 + i, 0x54320000 + i};
  }
  Input.gpr[3] = 0x200000; // bitmap-owned EC stop, never fetched as memory.
  Input.gpr[4] = reinterpret_cast<uintptr_t>(GuestStacks[0] + sizeof(GuestStacks[0]));
  std::atomic_uint Doorbell {};
  switchyard_fex_execution Execution {};
  Execution.size = sizeof(Execution);
  Execution.version = SWITCHYARD_FEX_EXECUTION_VERSION;
  Execution.suspend_doorbell = reinterpret_cast<uintptr_t>(&Doorbell);
  switchyard_fex_stop Stop {};
  Stop.size = sizeof(Stop);
  Stop.version = SWITCHYARD_FEX_STOP_VERSION;
  DetachedDispatch Dispatch {};
  DetachedObservation Observation {};
  const auto MakeInvocation = [&]() {
    return DetachedInvocation {OrdinaryControl ? Process->Signals.GetConfig().DispatcherBegin :
      reinterpret_cast<uintptr_t>(Dispatch.Entry), Dispatch.Frame,
      Frame->EnterCustomABI, Frame->LeaveCustomABI, CustomValue, StackTop, &Observation, os_custom_x18_abi_enabled};
  };
  const auto CheckIdle = [&]() {
    Check(!(switchyard_fex_admission_load(&Thread->Admission) & SWITCHYARD_FEX_ADMISSION_FLAGS) &&
          !ActiveExecutionThread && !Frame->ExternalSuspendDoorbell);
    Check(!os_custom_x18_abi_enabled());
  };
  const auto CheckOutput = [&]() {
    Output.size = sizeof(Output);
    Output.version = SWITCHYARD_FEX_STATE_VERSION;
    Require(switchyard_fex_thread_export_state(Thread, &Output));
    Check(!std::memcmp(Input.gpr, Output.gpr, sizeof(Input.gpr)));
    Check(!std::memcmp(Input.xmm, Output.xmm, sizeof(Input.xmm)));
    Check(!std::memcmp(Input.ymm_high, Output.ymm_high, sizeof(Input.ymm_high)));
    Check(Input.rflags == Output.rflags && Input.mxcsr == Output.mxcsr);
  };
  const auto Run = [&](uint32_t Reason, unsigned InternalTransitions, uint64_t RAX) {
    Require(switchyard_fex_thread_import_state(Thread, &Input));
    Require(PrepareDetachedDispatch(Thread, &Execution, Dispatch));
    auto Invocation = MakeInvocation();
    const auto Before = TransitionCount.load();
    InvokeDetached(&Invocation);
    Check(Observation.EntrySP == StackTop && Observation.ReturnSP == StackTop &&
          Observation.EntryX18 == CustomValue && Observation.ReturnX18 == CustomValue && Observation.CustomAfter == 1);
    Check(TransitionCount.load() - Before == InternalTransitions + 2); // outer fixture's pair is separate
    Require(CompleteExecution(Dispatch.Activation, &Stop));
    Check(Stop.reason == Reason);
    CheckIdle();
    const auto StopBefore = Stop;
    Check(CompleteExecution(Dispatch.Activation, &Stop) == SWITCHYARD_FEX_ERROR_BUSY);
    Check(!std::memcmp(&Stop, &StopBefore, sizeof(Stop)));
    Input.gpr[0] = RAX;
    CheckOutput();
  };

  Run(SWITCHYARD_FEX_STOP_EC_TRANSITION, 2, 17); // cold, real host compiler
  const auto StableEntry = Dispatch.Entry;
  const auto OuterRSP = Output.gpr[4];
  const auto OldActivation = Dispatch.Activation;
  for (unsigned i = 0; i < 32; ++i) Run(SWITCHYARD_FEX_STOP_EC_TRANSITION, 0, 17);
  // Quiescent native interval: these fail if either execution owner remains.
  Code[2] = 0x22;
  Require(switchyard_fex_process_invalidate_code(Process, Input.rip, 12));
  Require(switchyard_fex_thread_clear_code_cache(Thread));
  switchyard_fex_arm64_host_context Host {};
  Host.size = sizeof(Host);
  Host.version = SWITCHYARD_FEX_ARM64_HOST_CONTEXT_VERSION;
  Host.pc = Process->Signals.GetConfig().DispatcherBegin;
  uint64_t GuestRSP = 0xdead;
  Check(switchyard_fex_thread_query_jit_stack(Thread, &Host, 0, 0, &GuestRSP) == SWITCHYARD_FEX_ERROR_BUSY && GuestRSP == 0xdead);

  // A nested execution reuses the exact private stack top but a different
  // architectural guest RSP. No outer generated/C++ activation remains there.
  Input.rip = reinterpret_cast<uintptr_t>(Code + 12);
  Input.gpr[4] = reinterpret_cast<uintptr_t>(GuestStacks[1] + sizeof(GuestStacks[1]));
  Execution.expected_hlt_rip = Input.rip + 1;
  Run(SWITCHYARD_FEX_STOP_HLT, 2, 17);
  Check(Output.gpr[4] != OuterRSP && Dispatch.Entry == StableEntry && Dispatch.Activation.Generation > OldActivation.Generation);
  Input.rflags |= 0x100;
  Run(SWITCHYARD_FEX_STOP_SINGLE_STEP, 6, 17); // TF must bypass the warmed L1 block.
  Input.rflags &= ~uint64_t {0x100};
  Input.rip = reinterpret_cast<uintptr_t>(Code);
  Input.gpr[4] = OuterRSP;
  Execution.expected_hlt_rip = 0;
  Run(SWITCHYARD_FEX_STOP_EC_TRANSITION, 2, 34);
  Check(Dispatch.Entry == StableEntry);

  Require(switchyard_fex_thread_import_state(Thread, &Input));
  Require(PrepareDetachedDispatch(Thread, &Execution, Dispatch));
  const auto CurrentActivation = Dispatch.Activation;
  Check(CompleteExecution(OldActivation, &Stop) == SWITCHYARD_FEX_ERROR_BUSY && OwnsExecution(CurrentActivation));
  Stop.version++;
  Check(CompleteExecution(CurrentActivation, &Stop) == SWITCHYARD_FEX_ERROR_ABI_MISMATCH && OwnsExecution(CurrentActivation));
  Stop.version = SWITCHYARD_FEX_STOP_VERSION;
  std::thread WrongThread([&]() {
    switchyard_fex_stop OtherStop {};
    OtherStop.size = sizeof(OtherStop);
    OtherStop.version = SWITCHYARD_FEX_STOP_VERSION;
    Check(CompleteExecution(CurrentActivation, &OtherStop) == SWITCHYARD_FEX_ERROR_BUSY);
  });
  WrongThread.join();
  DetachedDispatch Untouched {OldActivation, nullptr, nullptr};
  Check(PrepareDetachedDispatch(Thread, &Execution, Untouched) == SWITCHYARD_FEX_ERROR_BUSY && !Untouched.Entry);
  Check(switchyard_fex_process_invalidate_code(Process, Input.rip, 12) == SWITCHYARD_FEX_ERROR_BUSY);
  Doorbell.store(1, std::memory_order_release);
  auto Invocation = MakeInvocation();
  InvokeDetached(&Invocation);
  Require(CompleteExecution(CurrentActivation, &Stop));
  Check(Stop.reason == SWITCHYARD_FEX_STOP_SUSPEND);
  CheckIdle();
  Doorbell.store(0, std::memory_order_release);
  Run(SWITCHYARD_FEX_STOP_EC_TRANSITION, 0, 34);

  // The custom caller skips only the outer mode pair. A true Mach-O callback
  // must still leave and reenter custom mode, even after compilation is warm.
  for (unsigned i = 0; i < 2; ++i) {
    Input.rip = reinterpret_cast<uintptr_t>(Code + 18);
    Input.gpr[0] = 7;
    Input.gpr[1] = 5;
    Execution.expected_hlt_rip = Input.rip + 2;
    Require(switchyard_fex_thread_import_state(Thread, &Input));
    Require(PrepareDetachedDispatch(Thread, &Execution, Dispatch));
    Invocation = MakeInvocation();
    const auto Before = TransitionCount.load();
    InvokeDetached(&Invocation);
    Check(Observation.ReturnSP == StackTop && Observation.ReturnX18 == CustomValue && Observation.CustomAfter == 1);
    Check(TransitionCount.load() - Before == (i ? 4U : 6U));
    Require(CompleteExecution(Dispatch.Activation, &Stop));
    Check(Stop.reason == SWITCHYARD_FEX_STOP_HLT);
    Input.gpr[0] = 0x12345678;
    Input.gpr[3] = 0x89abcdef;
    Input.gpr[1] = 0x11223344;
    Input.gpr[2] = 0x55667788;
    CheckIdle();
    CheckOutput();
  }
  Input.rip = reinterpret_cast<uintptr_t>(Code);
  Input.gpr[3] = 0x200000;
  Execution.expected_hlt_rip = 0;
  Run(SWITCHYARD_FEX_STOP_EC_TRANSITION, 0, 34);

  // Runtime authentication, independent of the system-mode preparation check.
  Require(PrepareDetachedDispatch(Thread, &Execution, Dispatch));
  Frame->CustomABIValue = CustomValue + 16;
  Invocation = MakeInvocation();
  InvokeDetached(&Invocation);
  Check(Observation.ReturnX18 == CustomValue && Observation.CustomAfter == 1);
  Check(CompleteExecution(Dispatch.Activation, &Stop) == SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
  Frame->CustomABIValue = 0;
  Check(PrepareDetachedDispatch(Thread, &Execution, Untouched) == SWITCHYARD_FEX_ERROR_UNSUPPORTED && !Untouched.Entry);
  CheckIdle();
  Frame->CustomABIValue = CustomValue;
  Run(SWITCHYARD_FEX_STOP_EC_TRANSITION, 0, 34);

  const auto LastAdmission = switchyard_fex_admission_load(&Thread->Admission);
  __atomic_store_n(&Thread->Admission.word, UINT64_MAX & ~SWITCHYARD_FEX_ADMISSION_FLAGS, __ATOMIC_RELEASE);
  Check(PrepareDetachedDispatch(Thread, &Execution, Untouched) == SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT && !Untouched.Entry);
  CheckIdle();
  __atomic_store_n(&Thread->Admission.word, LastAdmission, __ATOMIC_RELEASE); // fixture only, quiescent owner
  Execution.suspend_doorbell++;
  Check(PrepareDetachedDispatch(Thread, &Execution, Untouched) == SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT && !Untouched.Entry);
  CheckIdle();
  Execution.suspend_doorbell--;

  // Adapter-level nonlocal JIT fault. Reconstruction consumes the activation;
  // normal completion must reject. Full Wine exception handling remains open.
  struct sigaction Action {}, OldSEGV {}, OldBUS {};
  Action.sa_sigaction = DetachedFaultHandler;
  Action.sa_flags = SA_SIGINFO;
  sigemptyset(&Action.sa_mask);
  Check(sigaction(SIGSEGV, &Action, &OldSEGV) == 0 && sigaction(SIGBUS, &Action, &OldBUS) == 0);
  Input.rip = reinterpret_cast<uintptr_t>(Code + 14);
  Input.gpr[1] = 1;
  Require(switchyard_fex_thread_import_state(Thread, &Input));
  Require(PrepareDetachedDispatch(Thread, &Execution, Dispatch));
  Invocation = MakeInvocation();
  DetachedFaultArmed = 1;
  if (sigsetjmp(DetachedFaultJump, 1) == 0) {
    InvokeDetached(&Invocation);
    Check(false);
  }
  Check(!os_custom_x18_abi_enabled() && !DetachedFaultArmed && OwnsExecution(Dispatch.Activation));
  Output.size = sizeof(Output);
  Output.version = SWITCHYARD_FEX_STATE_VERSION;
  switchyard_fex_fault Fault {};
  Fault.size = sizeof(Fault);
  Fault.version = SWITCHYARD_FEX_FAULT_VERSION;
  Require(switchyard_fex_thread_reconstruct_jit_fault(Thread, &DetachedFaultContext,
    static_cast<uint32_t>(DetachedFaultSignal), 0, DetachedFaultAddress, &Output, &Fault));
  Check(Fault.guest_address == 1 && Output.rip == Input.rip && Output.gpr[4] == Input.gpr[4]);
  CheckIdle();
  CheckOutput();
  Check(CompleteExecution(Dispatch.Activation, &Stop) == SWITCHYARD_FEX_ERROR_BUSY);
  Check(sigaction(SIGSEGV, &OldSEGV, nullptr) == 0 && sigaction(SIGBUS, &OldBUS, nullptr) == 0);
  Input.rip = reinterpret_cast<uintptr_t>(Code);
  Run(SWITCHYARD_FEX_STOP_EC_TRANSITION, 0, 34);
  Check(munmap(Allocation, AllocationSize) == 0);
  std::printf("Detached dispatch: zero warm internal mode toggles, exact state/SP/x18, mutation + nested stack reuse, stale/wrong-thread/duplicate rejection, doorbell + nonlocal recovery\n");
}
