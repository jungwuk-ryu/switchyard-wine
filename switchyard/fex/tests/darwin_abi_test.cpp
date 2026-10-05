// SPDX-License-Identifier: MIT

// White-box core test: reuse the real adapter's initialization, ownership and
// feature detection without exporting experimental FEX C++ state in its C ABI.
#include "../src/switchyard_fex.cpp"
#include "Common/VectorRegType.h"
#include "Interface/Context/Context.h"
#include "Interface/Core/ArchHelpers/Arm64Emitter.h"
#include "Interface/Core/LookupCache.h"
#include "wine_signal_wrapper.h"

#include <os/arch/arm64.h>
#include <csignal>
#include <barrier>
#include <future>
#include <csetjmp>
#include <latch>
#include <source_location>
#include <thread>

namespace {

constexpr uint64_t CustomValue = 0x123456789ab000ULL;
std::atomic_uint TransitionCount {}, SignalCount {}, HostCount {}, FallbackCount {}, JITRuns {};
std::atomic_bool InjectSignals {};
bool OmitSystemTransition {};
bool SystemOnly {};
bool UnsetCustomValue {};
bool WineSignal {}, PublicOnly {}, OmitSignalWrapper {};
bool OmitFinalTransition {};
bool CheckWarmEntry {};
bool CheckDetached {}, DetachedOrdinaryControl {};
FEXCore::Core::CpuStateFrame* FinalTransitionFrame {};
static_assert(std::atomic_uint::is_always_lock_free && std::atomic_bool::is_always_lock_free);

// Patches are confined to owned, quiescent test code. No worker exists during
// this phase; immutable site metadata is published before executing any patch.
struct SignalSite {
  uintptr_t PC {};
  uint32_t Original {};
  bool Custom {}, HasCustomValue {}, Dispatcher {};
  std::atomic_bool Hit {};
};
std::array<SignalSite, 1024> SignalSites {};
std::atomic_uint PublishedSites {};
unsigned SiteCount {}, ObservedSites {};
constexpr uint32_t PatchTrap = 0xd420fc20U; // BRK #0x7e1, distinct from fixed traps.

uintptr_t ReadX18() {
  uintptr_t Value;
  __asm__ volatile("mov %0, x18" : "=r"(Value));
  return Value;
}

void Check(bool Condition, const std::source_location Location = std::source_location::current()) {
  if (Condition) return;
  if (os_custom_x18_abi_enabled()) os_set_custom_x18_abi_enabled(false);
  std::fprintf(stderr, "Darwin ABI assertion in %s at line %u\n", Location.function_name(), Location.line());
  std::_Exit(96);
}

__attribute__((naked)) void NoModeChange() { __asm__("ret"); }

// Exercise the generated transition helpers with independent host-ABI
// callee-saved canaries. The two traps mirror the post-return/pre-x18-load
// window; this does not substitute for Wine's signal reconstruction tests.
__attribute__((naked)) void ProbeHostRegisters(uint64_t, uint64_t, uint64_t, uint64_t*) {
  __asm__("sub sp, sp, #112\n\t"
          "stp x19, x20, [sp]\n\tstp x21, x22, [sp, #16]\n\t"
          "stp x23, x24, [sp, #32]\n\tstp x25, x26, [sp, #48]\n\t"
          "stp x27, x28, [sp, #64]\n\tstp x29, x30, [sp, #80]\n\t"
          "mrs x4, fpcr\n\tmrs x5, fpsr\n\tstp x4, x5, [sp, #96]\n\t"
          "mov x4, #0x400000\n\tmov x5, #0x8000000\n\tmsr fpcr, x4\n\tmsr fpsr, x5\n\t"
          "mov x19, #0x113\n\tmov x20, #0x114\n\tmov x21, #0x115\n\t"
          "mov x22, #0x116\n\tmov x23, #0x117\n\tmov x24, #0x118\n\t"
          "mov x25, #0x119\n\tmov x26, #0x11a\n\tmov x27, #0x11b\n\t"
          "mov x28, #0x11c\n\tblr x0\n\tbrk #0x7e0\n\t"
          "mov x18, x2\n\tbrk #0x7e0\n\tblr x1\n\t"
          "stp x19, x20, [x3]\n\tstp x21, x22, [x3, #16]\n\t"
          "stp x23, x24, [x3, #32]\n\tstp x25, x26, [x3, #48]\n\t"
          "stp x27, x28, [x3, #64]\n\t"
          "mrs x4, fpcr\n\tmrs x5, fpsr\n\tstp x4, x5, [x3, #80]\n\t"
          "ldp x4, x5, [sp, #96]\n\tmsr fpcr, x4\n\tmsr fpsr, x5\n\t"
          "ldp x19, x20, [sp]\n\tldp x21, x22, [sp, #16]\n\t"
          "ldp x23, x24, [sp, #32]\n\tldp x25, x26, [sp, #48]\n\t"
          "ldp x27, x28, [sp, #64]\n\tldp x29, x30, [sp, #80]\n\t"
          "add sp, sp, #112\n\tret");
}

void TransitionSignal(int Signal, siginfo_t*, void* Opaque) {
  auto* Context = static_cast<ucontext_t*>(Opaque);
  const bool Custom = os_custom_x18_abi_enabled();
  const uint64_t SavedX18 = Context->uc_mcontext->__ss.__x[18];
  if (Custom) os_set_custom_x18_abi_enabled(false);
  const auto PC = Context->uc_mcontext->__ss.__pc;
  if (Signal != SIGTRAP || *reinterpret_cast<const uint32_t*>(PC) != 0xd420fc00U) _exit(97);
  Context->uc_mcontext->__ss.__pc = PC + 4;
  SignalCount.fetch_add(1, std::memory_order_relaxed);
  if (Custom) {
    os_set_custom_x18_abi_enabled(true);
    __asm__ volatile("mov x18, %0" : : "r"(SavedX18));
  }
}

SignalSite* FindSignalSite(uintptr_t PC) {
  const auto Count = PublishedSites.load(std::memory_order_acquire);
  for (unsigned i = 0; i < Count; ++i) {
    if (SignalSites[i].PC == PC) return &SignalSites[i];
  }
  return nullptr;
}

void WineSignalPayload(int Signal, siginfo_t*, void* Opaque) {
  Check(!os_custom_x18_abi_enabled());
  auto* Context = static_cast<ucontext_t*>(Opaque);
  const auto PC = Context->uc_mcontext->__ss.__pc;
  Check(Signal == SIGTRAP);
  if (auto* Site = FindSignalSite(PC)) {
    Check(!Site->Hit.load() && *reinterpret_cast<const uint32_t*>(PC) == PatchTrap);
    if (Site->Dispatcher) {
      // A helper's inclusion in the delegator range is not guest provenance.
      // Exercise the real adapter query while execution accounting is active.
      switchyard_fex_arm64_host_context Host {};
      Host.size = sizeof(Host);
      Host.version = SWITCHYARD_FEX_ARM64_HOST_CONTEXT_VERSION;
      Host.pc = PC;
      uint64_t GuestRSP = 0xdead;
      Check(switchyard_fex_thread_query_jit_stack(ActiveExecutionThread, &Host, 0, 0, &GuestRSP) ==
            SWITCHYARD_FEX_ERROR_UNSUPPORTED && GuestRSP == 0xdead);
    }
    {
      FEXCore::Allocator::ScopedJITWrite Write;
      *reinterpret_cast<uint32_t*>(PC) = Site->Original;
      FEXCore::CPU::Arm64Emitter::ClearICache(reinterpret_cast<void*>(PC), sizeof(uint32_t));
    }
    Site->Hit.store(true, std::memory_order_release);
    // Do not advance PC: the interrupted LDR/LDP/BLR must execute exactly once.
  } else {
    Check(*reinterpret_cast<const uint32_t*>(PC) == 0xd420fc00U);
    Context->uc_mcontext->__ss.__pc = PC + 4;
  }
  SignalCount.fetch_add(1, std::memory_order_relaxed);
}

void WineTransitionSignal(int Signal, siginfo_t* Info, void* Opaque) {
  const auto* Context = static_cast<ucontext_t*>(Opaque);
  if (auto* Site = FindSignalSite(Context->uc_mcontext->__ss.__pc)) {
    Check(os_custom_x18_abi_enabled() == Site->Custom);
    if (Site->HasCustomValue) Check(Context->uc_mcontext->__ss.__x[18] == CustomValue);
  }
  if (OmitSignalWrapper) WineSignalPayload(Signal, Info, Opaque);
  else wine_test_x18_signal(WineSignalPayload, Signal, Info, Opaque);
}

// Test-only mode callback: no TLS, allocation or OS calls except the two APIs
// permitted by os/arch/arm64.h. Wine must supply its own established toggle and
// Unix-authenticated TEB when the separate PE bridge is integrated.
void Transition(bool Custom) {
  Check(os_custom_x18_abi_enabled() != Custom);
  if (!Custom) Check(ReadX18() == CustomValue);
  if (InjectSignals.load(std::memory_order_relaxed)) __asm__ volatile("brk #0x7e0");
  if (WineSignal) wine_test_x18_transition(Custom);
  else os_set_custom_x18_abi_enabled(Custom);
  if (InjectSignals.load(std::memory_order_relaxed)) __asm__ volatile("brk #0x7e0");
  TransitionCount.fetch_add(1, std::memory_order_relaxed);
  // Exercise the gate's preservation, not the current OS setter's incidental
  // register footprint. The ordinary C ABI permits these clobbers.
  __asm__ volatile("movi v0.16b, #0xaa\n\tmovi v1.16b, #0xbb\n\t"
                   "movi v8.16b, #0xee\n\t"
                   "movi v16.16b, #0xcc\n\tmovi v31.16b, #0xdd\n\t"
                   "mov x8, #0x18\n\tmov x9, #0x19\n\tmov x16, #0x20\n\t"
                   "mov x17, #0x21\n\tcmp x8, x9\n\t"
                   "mov x19, #0\n\tmov x20, #0\n\tmov x21, #0\n\tmov x22, #0\n\t"
                   "mov x23, #0\n\tmov x24, #0\n\tmov x25, #0\n\tmov x26, #0\n\t"
                   "mov x27, #0\n\tmov x28, #0\n\tmsr fpcr, xzr\n\tmsr fpsr, xzr"
                   : : : "v0", "v1", "v8", "v16", "v31", "x8", "x9", "x16", "x17", "cc",
                         "x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28");
}

__uint128_t HostCPUID(void*, uint64_t Function, uint64_t Leaf) {
  Check(!os_custom_x18_abi_enabled());
  Check(Function == 7 && Leaf == 5);
  if (OmitFinalTransition) FinalTransitionFrame->LeaveCustomABI = reinterpret_cast<uintptr_t>(NoModeChange);
  HostCount.fetch_add(1, std::memory_order_relaxed);
  return (__uint128_t {0x5566778811223344ULL} << 64) | 0x89abcdef12345678ULL;
}

class HostCallProbe final : public FEXCore::CPU::Arm64Emitter {
public:
  HostCallProbe(FEXCore::Context::Context* Context, void* Buffer, size_t Size)
    : Arm64Emitter(static_cast<FEXCore::Context::ContextImpl*>(Context), Buffer, Size) {
    PushCalleeSavedRegisters();
    mov(FEXCore::CPU::STATE, ARMEmitter::XReg::x0);
    EnterCustomABI();
    mov(ARMEmitter::XReg::x0, 0);
    mov(ARMEmitter::XReg::x1, 7);
    mov(ARMEmitter::XReg::x2, 5);
    LoadConstant(ARMEmitter::Size::i64Bit, ARMEmitter::Reg::r3, reinterpret_cast<uintptr_t>(HostCPUID));
    CallHostFunction(ARMEmitter::Reg::r3);
    LeaveCustomABI();
    PopCalleeSavedRegisters();
    ret();
    ClearICache(Buffer, GetCursorOffset());
  }
};

__attribute__((preserve_all)) FEXCore::VectorRegType HostF80(
    uint16_t FCW, FEXCore::VectorRegType Value, FEXCore::Core::CpuStateFrame* Frame) {
  Check(!os_custom_x18_abi_enabled() && FCW == 0x37f && Frame != nullptr);
  FallbackCount.fetch_add(1, std::memory_order_relaxed);
  return Value;
}

struct ExecutableFixture {
  const uint8_t* Code;
  size_t Size;
};

switchyard_fex_result QueryCode(void* Opaque, uint64_t Address, switchyard_fex_executable_range* Range) {
  Check(!os_custom_x18_abi_enabled()); // Includes cold CompileBlock/linker calls.
  const auto* Fixture = static_cast<ExecutableFixture*>(Opaque);
  const auto Base = reinterpret_cast<uintptr_t>(Fixture->Code);
  if (Address < Base || Address - Base >= Fixture->Size) return SWITCHYARD_FEX_ERROR_GUEST_FAULT;
  Range->base = Base;
  Range->length = Fixture->Size;
  Range->flags = 0;
  return SWITCHYARD_FEX_OK;
}

void Require(switchyard_fex_result Result, const std::source_location Location = std::source_location::current()) {
  if (Result == SWITCHYARD_FEX_OK) return;
  std::fprintf(stderr, "Darwin ABI test: %s in %s at line %u\n",
               switchyard_fex_result_string(Result), Location.function_name(), Location.line());
  std::exit(1);
}

void RunCase(switchyard_fex_thread* Thread, const uint8_t* Code, size_t Size,
             unsigned Kind, bool Signals, bool ManageSignals = true) {
  alignas(16) uint8_t Stack[4096] {};
  alignas(16) uint8_t Result[16] {};
  switchyard_fex_x64_state Input {}, Output {};
  Input.size = sizeof(Input);
  Input.version = SWITCHYARD_FEX_STATE_VERSION;
  Input.flags = SWITCHYARD_FEX_STATE_YMM_HIGH_VALID;
  Input.rip = reinterpret_cast<uintptr_t>(Code);
  Input.rflags = 0x246;
  if (Kind == 4) Input.rflags |= 0x100;
  Input.mxcsr = 0x1f80;
  Input.fcw = 0x37f;
  Input.segment[1] = FEXCore::Core::CPUState::DEFAULT_USER_CS << 3;
  for (size_t i = 0; i < 16; ++i) {
    Input.gpr[i] = 0x8000 + i;
    Input.xmm[i] = {0x12340000 + i, 0x56780000 + i};
    Input.ymm_high[i] = {0x98760000 + i, 0x54320000 + i};
  }
  Input.gpr[4] = reinterpret_cast<uintptr_t>(Stack + sizeof(Stack));
  if (Kind == 0) { Input.gpr[0] = 7; Input.gpr[1] = 5; }
  if (Kind == 1) Input.gpr[0] = reinterpret_cast<uintptr_t>(Result);
  if (Kind == 2) { Input.gpr[0] = 0; Input.gpr[2] = 1; Input.gpr[1] = 3; }
  Require(switchyard_fex_thread_import_state(Thread, &Input));
  std::atomic_uint Doorbell {};
  switchyard_fex_execution Execution {};
  Execution.size = sizeof(Execution);
  Execution.version = SWITCHYARD_FEX_EXECUTION_VERSION;
  Execution.expected_hlt_rip = Input.rip + Size - 1;
  Execution.suspend_doorbell = reinterpret_cast<uintptr_t>(&Doorbell);
  switchyard_fex_stop Stop {};
  Stop.size = sizeof(Stop);
  Stop.version = SWITCHYARD_FEX_STOP_VERSION;
  if (ManageSignals) InjectSignals.store(Signals, std::memory_order_relaxed);
  const auto InitialSystemX18 = ReadX18();
  const auto Status = switchyard_fex_thread_execute(Thread, &Execution, &Stop);
  if (UnsetCustomValue) {
    Check(Status == SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT && Stop.reason == SWITCHYARD_FEX_STOP_INTERNAL);
    Check(!os_custom_x18_abi_enabled() && TransitionCount.load() == 0 && HostCount.load() == 0);
    Check(!(switchyard_fex_admission_load(&Thread->Admission) & SWITCHYARD_FEX_ADMISSION_FLAGS));
    return;
  }
  if (OmitFinalTransition) {
    Check(!os_custom_x18_abi_enabled() && Status == SWITCHYARD_FEX_ERROR_INTERNAL &&
          Stop.reason == SWITCHYARD_FEX_STOP_INTERNAL && Stop.rip == Execution.expected_hlt_rip);
    Check(!(switchyard_fex_admission_load(&Thread->Admission) & SWITCHYARD_FEX_ADMISSION_FLAGS) &&
          ActiveExecutionThread == nullptr);
    return;
  }
  if (Status != SWITCHYARD_FEX_OK) {
    const auto& Fault = Thread->CoreThread->CurrentFrame->SynchronousFaultData;
    std::fprintf(stderr, "case %u stop=%u fault=%u trap=%u custom=%u system-x18-changed=%u\n",
                 Kind, Stop.reason, Fault.Signal, Fault.TrapNo, os_custom_x18_abi_enabled(), ReadX18() != InitialSystemX18);
  }
  Require(Status);
  JITRuns.fetch_add(1, std::memory_order_relaxed);
  if (ManageSignals) InjectSignals.store(false, std::memory_order_relaxed);
  Check(!os_custom_x18_abi_enabled());
  if (Kind == 4) Check(Stop.reason == SWITCHYARD_FEX_STOP_SINGLE_STEP && Stop.rip == Input.rip + 1);
  else Check(Stop.reason == SWITCHYARD_FEX_STOP_HLT);
  Output.size = sizeof(Output);
  Output.version = SWITCHYARD_FEX_STATE_VERSION;
  Require(switchyard_fex_thread_export_state(Thread, &Output));
  if (Kind == 0) {
    Input.gpr[0] = 0x12345678; Input.gpr[3] = 0x89abcdef;
    Input.gpr[1] = 0x11223344; Input.gpr[2] = 0x55667788;
  }
  if (Kind == 1) {
    uint64_t Low;
    uint16_t High;
    std::memcpy(&Low, Result, sizeof(Low));
    std::memcpy(&High, Result + 8, sizeof(High));
    Check(Low == 0x8000000000000000ULL && High == 0x3fff);
  }
  if (Kind == 2) { Input.gpr[0] = 0x5555555555555555ULL; Input.gpr[2] = 1; }
  for (size_t i = 0; i < 16; ++i) {
    if (Input.gpr[i] != Output.gpr[i])
      std::fprintf(stderr, "case %u GPR[%zu]: expected %llx, got %llx\n", Kind, i,
                   static_cast<unsigned long long>(Input.gpr[i]), static_cast<unsigned long long>(Output.gpr[i]));
  }
  Check(!std::memcmp(Input.gpr, Output.gpr, sizeof(Input.gpr)));
  Check(!std::memcmp(Input.xmm, Output.xmm, sizeof(Input.xmm)));
  Check(!std::memcmp(Input.ymm_high, Output.ymm_high, sizeof(Input.ymm_high)));
  if (Kind != 2) Check(Input.rflags == Output.rflags); // DIV leaves flags undefined.
  Check(Input.mxcsr == Output.mxcsr);
}

void AddSignalSite(uintptr_t PC, bool Custom, bool HasCustomValue, bool Dispatcher) {
  Check(!(PC & 3));
  for (unsigned i = 0; i < SiteCount; ++i) {
    if (SignalSites[i].PC != PC) continue;
    Check(SignalSites[i].Custom == Custom && SignalSites[i].Dispatcher == Dispatcher);
    SignalSites[i].HasCustomValue |= HasCustomValue;
    return;
  }
  Check(SiteCount < SignalSites.size());
  auto& Site = SignalSites[SiteCount++];
  Site.PC = PC;
  Site.Original = *reinterpret_cast<const uint32_t*>(PC);
  Check(Site.Original != PatchTrap);
  Site.Custom = Custom;
  Site.HasCustomValue = HasCustomValue;
  Site.Dispatcher = Dispatcher;
  Site.Hit.store(false);
}

void CollectTransitionSites(uintptr_t Begin, uintptr_t End, bool Dispatcher) {
  using Frame = FEXCore::Core::CpuStateFrame;
  constexpr auto LoadFrame = [](unsigned Register, size_t Offset) {
    return 0xf9400000U | (static_cast<uint32_t>(Offset / 8) << 10) |
           (FEXCore::CPU::STATE.Idx() << 5) | Register;
  };
  static_assert(offsetof(Frame, EnterCustomABI) % 8 == 0 && offsetof(Frame, LeaveCustomABI) % 8 == 0 &&
                offsetof(Frame, CustomABIValue) % 8 == 0 && offsetof(Frame, LeaveCustomABI) / 8 < 4096);
  Check(Begin < End && End - Begin <= 1024 * 1024 && !((Begin | End) & 3));
  for (auto PC = Begin; End - PC >= 16; PC += 4) {
    const auto* Words = reinterpret_cast<const uint32_t*>(PC);
    if (Words[1] != 0xd63f03c0U) continue; // BLR LR following a frame helper load.
    if (Words[0] == LoadFrame(30, offsetof(Frame, EnterCustomABI))) {
      Check(Words[2] == LoadFrame(18, offsetof(Frame, CustomABIValue)) || Words[2] == 0xa8c17bf2U);
      AddSignalSite(PC, false, false, Dispatcher);
      AddSignalSite(PC + 4, false, false, Dispatcher);
      AddSignalSite(PC + 8, true, false, Dispatcher); // Mode changed, x18 not loaded yet.
      AddSignalSite(PC + 12, true, true, Dispatcher);
    } else if (Words[0] == LoadFrame(30, offsetof(Frame, LeaveCustomABI))) {
      AddSignalSite(PC, true, true, Dispatcher);
      AddSignalSite(PC + 4, true, true, Dispatcher);
      AddSignalSite(PC + 8, false, false, Dispatcher);
    }
  }
}

void CollectModeHelper(uintptr_t Begin, uintptr_t End, bool Enter) {
  Check(Begin < End && End - Begin <= 1024 * 1024);
  bool AfterCall = false;
  for (auto PC = Begin; PC < End && PC - Begin < 4096; PC += 4) {
    const auto Word = *reinterpret_cast<const uint32_t*>(PC);
    const bool Custom = AfterCall ? Enter : !Enter;
    AddSignalSite(PC, Custom, Custom && !Enter, true);
    if (Word == 0xd65f03c0U) { Check(AfterCall); return; } // RET
    if (Word == 0xd63f0020U) { Check(!AfterCall); AfterCall = true; } // BLR x1
  }
  Check(false); // No bounded, complete generated helper found.
}

void RunSignalPCs(switchyard_fex_thread* Thread, ExecutableFixture& Current,
                  const ExecutableFixture* Fixtures) {
  const auto& Config = Thread->Process->Signals.GetConfig();
  auto* Core = Thread->CoreThread;
  auto* Frame = Core->CurrentFrame;
  auto* Context = Thread->Process->Context.get();
  for (unsigned Kind = 0; Kind < 3; ++Kind) {
    Current = Fixtures[Kind];
    RunCase(Thread, Current.Code, Current.Size, Kind, false);
    SiteCount = 0;
    CollectTransitionSites(Config.DispatcherBegin, Config.DispatcherEnd, true);
    const auto HelperBegin = SiteCount;
    CollectModeHelper(Frame->EnterCustomABI, Config.DispatcherEnd, true);
    CollectModeHelper(Frame->LeaveCustomABI, Config.DispatcherEnd, false);
    const auto HelperEnd = SiteCount;
    const auto Begin = Frame->State.InlineJITBlockHeader;
    Check(Begin && Begin <= UINTPTR_MAX - 1024 * 1024);
    // Use the core's validated current-block bounds; never trust a tail offset
    // or scan neighboring mappings. The four-byte header is not executable.
    auto End = Begin + 4;
    while (End - Begin < 1024 * 1024 && Context->IsHostAddressInCurrentBlock(Core, End, 4)) End += 4;
    Check(End > Begin + 4 && End - Begin < 1024 * 1024);
    CollectTransitionSites(Begin + 4, End, false);
    {
      FEXCore::Allocator::ScopedJITWrite Write;
      for (unsigned i = 0; i < SiteCount; ++i) {
        const auto& Site = SignalSites[i];
        *reinterpret_cast<uint32_t*>(Site.PC) = PatchTrap;
        FEXCore::CPU::Arm64Emitter::ClearICache(reinterpret_cast<void*>(Site.PC), sizeof(uint32_t));
      }
    }
    PublishedSites.store(SiteCount, std::memory_order_release);
    RunCase(Thread, Current.Code, Current.Size, Kind, false);
    PublishedSites.store(0, std::memory_order_release);
    unsigned Hits = 0, JITHits = 0, CustomHits = 0, SystemHits = 0;
    {
      FEXCore::Allocator::ScopedJITWrite Write;
      for (unsigned i = 0; i < SiteCount; ++i) {
        const auto& Site = SignalSites[i];
        if (Site.Hit.load(std::memory_order_acquire)) {
          ++Hits;
          JITHits += !Site.Dispatcher;
          CustomHits += Site.Custom;
          SystemHits += !Site.Custom;
          Check(*reinterpret_cast<const uint32_t*>(Site.PC) == Site.Original);
        } else {
          Check(*reinterpret_cast<const uint32_t*>(Site.PC) == PatchTrap);
          *reinterpret_cast<uint32_t*>(Site.PC) = Site.Original;
          FEXCore::CPU::Arm64Emitter::ClearICache(reinterpret_cast<void*>(Site.PC), sizeof(uint32_t));
        }
      }
    }
    // Both helpers must be interrupted at every instruction. CPUID also calls
    // from its JIT body; F80 and long division call through dispatcher thunks.
    for (auto i = HelperBegin; i < HelperEnd; ++i) Check(SignalSites[i].Hit.load());
    Check(Hits > HelperEnd - HelperBegin && CustomHits && SystemHits && (Kind != 0 || JITHits));
    ObservedSites += Hits;
    std::printf("Wine signal fixture %u: %u/%u generated PCs resumed (%u JIT, %u custom, %u system)\n",
                Kind, Hits, SiteCount, JITHits, CustomHits, SystemHits);
  }
}

#include "detached_dispatch_test.h"
#include "vector_state_test.h"
#include "admission_test.h"
#include "shared_admission_test.h"

} // namespace

int main(int Argc, char** Argv) {
  if (__builtin_available(macOS 26.4, *)) {} else return 77;
  if (Argc == 2 && (!std::strcmp(Argv[1], "--vector-state") || !std::strcmp(Argv[1], "--admission") ||
                    !std::strcmp(Argv[1], "--shared-admission"))) {
    switchyard_fex_config Config {};
    Config.size = sizeof(Config);
    Config.abi_version = SWITCHYARD_FEX_ABI_VERSION;
    if (!std::strcmp(Argv[1], "--shared-admission")) Config.flags = SWITCHYARD_FEX_CONFIG_EXTERNAL_ADMISSION;
    switchyard_fex_process* Process {};
    Require(switchyard_fex_process_create(&Config, &Process));
    if (!std::strcmp(Argv[1], "--admission")) RunAdmissionTests(Process);
    else if (!std::strcmp(Argv[1], "--shared-admission")) RunSharedAdmissionTests(Process);
    else RunVectorStateTests(Process);
    Require(switchyard_fex_process_destroy(Process));
    return 0;
  }
  if (Argc == 2 && !std::strcmp(Argv[1], "--omit-system-transition")) OmitSystemTransition = true;
  else if (Argc == 2 && !std::strcmp(Argv[1], "--system-only")) SystemOnly = true;
  else if (Argc == 2 && !std::strcmp(Argv[1], "--wine-signal")) WineSignal = true;
  else if (Argc == 2 && !std::strcmp(Argv[1], "--wine-signal-public")) WineSignal = PublicOnly = true;
  else if (Argc == 2 && !std::strcmp(Argv[1], "--wine-signal-unwrapped")) WineSignal = OmitSignalWrapper = true;
  else if (Argc == 2 && !std::strcmp(Argv[1], "--omit-final-system-transition")) OmitFinalTransition = true;
  else if (Argc == 2 && !std::strcmp(Argv[1], "--check-warm-entry")) CheckWarmEntry = true;
  else if (Argc == 2 && !std::strcmp(Argv[1], "--detached-dispatch")) CheckDetached = WineSignal = true;
  else if (Argc == 2 && !std::strcmp(Argv[1], "--public-detached-dispatch")) WineSignal = true;
  else if (Argc == 2 && !std::strcmp(Argv[1], "--detached-ordinary-entry")) CheckDetached = WineSignal = DetachedOrdinaryControl = true;
  else Check(Argc == 1);
  if (WineSignal) {
    Check(wine_test_x18_init(PublicOnly));
    std::printf("Wine signal mode policy: %s\n", wine_test_x18_idempotent() ? "idempotent" : "public strict toggle");
  }
  struct sigaction Action {}, Previous {};
  Action.sa_sigaction = WineSignal ? WineTransitionSignal : TransitionSignal;
  Action.sa_flags = SA_SIGINFO;
  sigemptyset(&Action.sa_mask);
  Check(sigaction(SIGTRAP, &Action, &Previous) == 0);

  if (Argc == 2 && !std::strcmp(Argv[1], "--public-detached-dispatch")) {
    RunPublicDispatchTests();
    Check(sigaction(SIGTRAP, &Previous, nullptr) == 0);
    return 0;
  }

  switchyard_fex_config Config {};
  Config.size = sizeof(Config);
  Config.abi_version = SWITCHYARD_FEX_ABI_VERSION;
  Config.flags = SWITCHYARD_FEX_CONFIG_MULTIBLOCK;
  switchyard_fex_process* Process {};
  Require(switchyard_fex_process_create(&Config, &Process));
  // No threads exist: replace only this test process's private core so the
  // callback counters can instrument generated host calls independently.
  Process->Context.reset();
  const auto Features = DetectHostFeatures();
  Check(Features.has_value());
  Process->Context = FEXCore::Context::Context::CreateNewContext(*Features);
  // The CLZ/bit-5 generator supports exactly the Windows 4-GiB low domain.
  // Check the real core setter independently of the adapter's config gate,
  // including failed replacement of a previously accepted geometry.
  Check(Process->Context->SetGuestMemoryMapping(uint64_t {1} << 32, uint64_t {1} << 40));
  const auto* MappingContext = static_cast<FEXCore::Context::ContextImpl*>(Process->Context.get());
  for (unsigned Shift = 0; Shift < 64; ++Shift) {
    if (Shift == 32) continue;
    Check(!Process->Context->SetGuestMemoryMapping(uint64_t {1} << Shift, uint64_t {1} << 40));
    Check(MappingContext->Config.GuestMemoryLowAddressLimit == (uint64_t {1} << 32));
    Check(MappingContext->Config.GuestMemoryHostOffset == (uint64_t {1} << 40));
  }
  Check(Process->Context->SetGuestMemoryMapping(0, 0));
  Process->Context->SetSignalDelegator(&Process->Signals);
  Process->Context->SetSyscallHandler(&Process->Syscalls);
  Check(!Process->Context->SetCustomABITransition(Transition)); // External stops required.
  alignas(8) std::array<uint64_t, 16> DetachedBitmap {};
  DetachedBitmap[8] = 1; // EC target 0x200000 at guest page shift12.
  Check(CheckDetached ? Process->Context->SetExternalExecutionStops(DetachedBitmap.data(), 0x3fffff, 12) :
                        Process->Context->SetExternalExecutionStops(nullptr, 0, 0));
  Check(Process->Context->SetCustomABITransition(SystemOnly ? nullptr : Transition));
  Process->Context->EnableExitOnHLT();
  Check(Process->Context->InitCore());
  Check(!Process->Context->SetCustomABITransition(nullptr));

  constexpr uint8_t CPUIDCode[] = {0x0f, 0xa2, 0xf4};
  constexpr uint8_t F80Code[] = {0xd9, 0xe8, 0xd9, 0xfe, 0xdb, 0x38, 0xf4};
  constexpr uint8_t DivideCode[] = {0x48, 0xf7, 0xf1, 0xf4};
  const ExecutableFixture Fixtures[] = {{CPUIDCode, sizeof(CPUIDCode)},
                                       {F80Code, sizeof(F80Code)},
                                       {DivideCode, sizeof(DivideCode)}};
  ExecutableFixture Current {};
  Require(switchyard_fex_process_set_executable_range_query(Process, QueryCode, &Current));
  switchyard_fex_thread* Thread {};
  Require(switchyard_fex_thread_create(Process, &Thread));
  if (!SystemOnly) {
    const auto& Signals = Process->Signals.GetConfig();
    for (auto Address : {Thread->CoreThread->CurrentFrame->EnterCustomABI,
                         Thread->CoreThread->CurrentFrame->LeaveCustomABI}) {
      Check(Address >= Signals.DispatcherBegin && Address < Signals.DispatcherEnd && !(Address & 15));
    }
    // A missing authenticated value is rejected while still in system mode,
    // before guest code or a host callback can run. The same thread recovers.
    UnsetCustomValue = true;
    Current = Fixtures[0];
    RunCase(Thread, Current.Code, Current.Size, 0, false);
    Process->Context->HandleCallback(Thread->CoreThread, reinterpret_cast<uintptr_t>(Current.Code));
    Check(Thread->CoreThread->CurrentFrame->ExternalStop == FEXCore::Core::ExternalStopReason::InvalidCustomABI);
    Check(!os_custom_x18_abi_enabled() && TransitionCount.load() == 0);
    UnsetCustomValue = false;
  }
  Thread->CoreThread->CurrentFrame->CustomABIValue = CustomValue;
  if (!SystemOnly) {
    std::array<uint64_t, 12> Registers {};
    const auto* Frame = Thread->CoreThread->CurrentFrame;
    ProbeHostRegisters(Frame->EnterCustomABI, Frame->LeaveCustomABI, CustomValue, Registers.data());
    Check(!os_custom_x18_abi_enabled());
    for (size_t i = 0; i < 10; ++i) Check(Registers[i] == 0x113 + i);
    Check(Registers[10] == 0x400000 && Registers[11] == 0x8000000);
  }
  Thread->CoreThread->CurrentFrame->Pointers.CPUIDFunction = reinterpret_cast<uintptr_t>(HostCPUID);
  Thread->CoreThread->CurrentFrame->Pointers.FallbackHandlerPointers[FEXCore::Core::OPINDEX_F80SIN].Func =
    reinterpret_cast<uintptr_t>(HostF80);
  if (CheckDetached) {
    RunDetachedTests(Thread, Current, DetachedOrdinaryControl);
    Require(switchyard_fex_thread_destroy(Thread));
    Require(switchyard_fex_process_destroy(Process));
    Check(sigaction(SIGTRAP, &Previous, nullptr) == 0);
    return 0;
  }
  if (CheckWarmEntry) {
    constexpr uint8_t Leaf[] = {0x90, 0xf4}; // NOP; HLT, no guest host calls.
    Current = {Leaf, sizeof(Leaf)};
    unsigned LeafRun = 0;
    auto RunLeaf = [&](unsigned ExpectedTransitions, unsigned Kind = 3) {
      ++LeafRun;
      const auto Before = TransitionCount.load();
      RunCase(Thread, Current.Code, Current.Size, Kind, false);
      const auto Count = TransitionCount.load() - Before;
      if (Count != ExpectedTransitions)
        std::fprintf(stderr, "leaf entry %u (kind %u): expected %u mode transitions, got %u\n",
                     LeafRun, Kind, ExpectedTransitions, Count);
      Check(Count == ExpectedTransitions);
    };
    RunLeaf(4); // Cold compile: entry, host compiler exit/reentry, final exit.
    for (unsigned i = 0; i < 32; ++i) RunLeaf(2); // Warm: no host compiler call.

    auto& CPU = Thread->CoreThread->CurrentFrame->State;
    const auto RIP = reinterpret_cast<uintptr_t>(Leaf);
    const auto Offset = (RIP & (CPU.L1Mask >> 4)) * sizeof(FEXCore::LookupCache::LookupCacheEntry);
    auto* Entry = reinterpret_cast<FEXCore::LookupCache::LookupCacheEntry*>(CPU.L1Pointer + Offset);
    Check(Entry->GuestCode == RIP && Entry->HostCode != 0);
    Entry->GuestCode ^= (CPU.L1Mask >> 4) + 1; // Colliding index, wrong full tag.
    RunLeaf(4);
    Check(Entry->GuestCode == RIP && Entry->HostCode != 0);
    Entry->HostCode = 0; // Exact tag must not permit a null branch target.
    RunLeaf(4);
    RunLeaf(2);
    // TF compiles NOP, visits the exit linker, then compiles the next entry
    // whose pre-instruction trap stops before HLT: three host-call pairs.
    RunLeaf(8, 4); // Bypass the warmed whole block and stop after NOP.
    RunLeaf(2);
    Require(switchyard_fex_process_invalidate_code(Process, RIP, sizeof(Leaf)));
    RunLeaf(4);
    RunLeaf(2);
    Require(switchyard_fex_thread_clear_code_cache(Thread));
    RunLeaf(4);
    RunLeaf(2);
    Check(!(switchyard_fex_admission_load(&Thread->Admission) & SWITCHYARD_FEX_ADMISSION_FLAGS));
    Require(switchyard_fex_thread_destroy(Thread));
    Require(switchyard_fex_process_destroy(Process));
    Check(sigaction(SIGTRAP, &Previous, nullptr) == 0);
    std::puts("Darwin entry: warm host-compiler calls eliminated; tag/null misses, TF, invalidation and clear exact");
    return 0;
  }
  if (OmitFinalTransition) {
    FinalTransitionFrame = Thread->CoreThread->CurrentFrame;
    const auto Leave = FinalTransitionFrame->LeaveCustomABI;
    Current = Fixtures[0];
    RunCase(Thread, Current.Code, Current.Size, 0, false);
    FinalTransitionFrame->LeaveCustomABI = Leave;
    OmitFinalTransition = false;
    RunCase(Thread, Current.Code, Current.Size, 0, false);
    Check(HostCount.load() == 2 && !os_custom_x18_abi_enabled());
    Require(switchyard_fex_thread_destroy(Thread));
    Require(switchyard_fex_process_destroy(Process));
    Check(sigaction(SIGTRAP, &Previous, nullptr) == 0);
    std::puts("Darwin ABI: omitted final exit rejected in system mode; accounting and same-thread reuse exact");
    return 0;
  }
  if (OmitSystemTransition) {
    // A direct host-call emitter probe reaches the checked callback before
    // compiler/libc without relying on dispatcher cache behavior.
    constexpr size_t ProbeCapacity = 1024;
    void* ProbeCode = FEXCore::Allocator::VirtualAlloc(ProbeCapacity, true);
    Check(ProbeCode != MAP_FAILED);
    {
      FEXCore::Allocator::ScopedJITWrite Write;
      HostCallProbe Probe(Process->Context.get(), ProbeCode, ProbeCapacity);
    }
    using ProbeEntry = void (*)(FEXCore::Core::CpuStateFrame*);
    const auto Entry = reinterpret_cast<ProbeEntry>(ProbeCode);
    Entry(Thread->CoreThread->CurrentFrame);
    Check(HostCount.load() == 1 && !os_custom_x18_abi_enabled());
    Thread->CoreThread->CurrentFrame->LeaveCustomABI = reinterpret_cast<uintptr_t>(NoModeChange);
    Entry(Thread->CoreThread->CurrentFrame);
    FEXCore::Allocator::VirtualFree(ProbeCode, ProbeCapacity);
    Check(false);
  }
  for (unsigned Kind = 0; Kind < 3; ++Kind) {
    Current = Fixtures[Kind];
    for (unsigned i = 0; i < 32; ++i) {
      RunCase(Thread, Current.Code, Current.Size, Kind, (i & 1) != 0);
    }
  }
  Check(HostCount.load() == 32 && FallbackCount.load() == 32);
  if (WineSignal) RunSignalPCs(Thread, Current, Fixtures);
  Check(SystemOnly ? TransitionCount.load() == 0 : SignalCount.load() > 0);
  Check((TransitionCount.load() & 1) == 0 &&
        !(switchyard_fex_admission_load(&Thread->Admission) & SWITCHYARD_FEX_ADMISSION_FLAGS));
  Require(switchyard_fex_thread_destroy(Thread));

  Current = Fixtures[0];
  std::array<switchyard_fex_thread*, 8> Threads {};
  std::array<std::thread, 8> Workers;
  std::latch Ready {8}, Start {1};
  for (auto& WorkerThread : Threads) {
    Require(switchyard_fex_thread_create(Process, &WorkerThread));
    WorkerThread->CoreThread->CurrentFrame->CustomABIValue = CustomValue;
    WorkerThread->CoreThread->CurrentFrame->Pointers.CPUIDFunction = reinterpret_cast<uintptr_t>(HostCPUID);
  }
  InjectSignals.store(true, std::memory_order_relaxed);
  for (size_t i = 0; i < Workers.size(); ++i) {
    Workers[i] = std::thread([&, i]() {
      Ready.count_down();
      Start.wait();
      for (unsigned j = 0; j < 8; ++j) RunCase(Threads[i], Current.Code, Current.Size, 0, true, false);
    });
  }
  Ready.wait();
  Start.count_down();
  for (auto& Worker : Workers) Worker.join();
  InjectSignals.store(false, std::memory_order_relaxed);
  for (auto* WorkerThread : Threads) Require(switchyard_fex_thread_destroy(WorkerThread));
  Check(HostCount.load() == 96 + (WineSignal ? 2U : 0U) && Process->Threads.empty());
  Check((TransitionCount.load() & 1) == 0 && !os_custom_x18_abi_enabled());
  Require(switchyard_fex_process_destroy(Process));
  Check(sigaction(SIGTRAP, &Previous, nullptr) == 0);
  std::printf("Darwin ABI: %u JIT runs, 8 concurrent threads, %u transitions, %u injected signals, %u generated-PC checks; normal/preserve-all results exact\n",
              JITRuns.load(), TransitionCount.load(), SignalCount.load(), ObservedSites);
}
