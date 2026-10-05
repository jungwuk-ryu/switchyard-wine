// SPDX-License-Identifier: MIT

#include "switchyard_fex.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/ArchHelpers/Arm64.h>
#include <FEXCore/Utils/AllocatorHooks.h>
#include <FEXCore/Utils/LogManager.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <type_traits>
#include <vector>

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>
#ifdef SWITCHYARD_FEX_HAS_CUSTOM_X18_API
# include <os/arch/arm64.h>
#endif

#ifndef SWITCHYARD_FEX_UPSTREAM_REVISION
# define SWITCHYARD_FEX_UPSTREAM_REVISION "Unknown"
#endif

namespace {

constexpr uint32_t KnownConfigFlags = SWITCHYARD_FEX_CONFIG_MULTIBLOCK |
                                      SWITCHYARD_FEX_CONFIG_CUSTOM_DISPATCH |
                                      SWITCHYARD_FEX_CONFIG_EXTERNAL_ADMISSION;
constexpr uint32_t KnownStateFlags = SWITCHYARD_FEX_STATE_YMM_HIGH_VALID;
constexpr uint32_t ValidMXCSRMask = 0x0000FFFFU;

struct InitializationFailure {
  const char* Stage;
};

static_assert(sizeof(FEXCore::Core::CPUState::gregs) ==
              sizeof(switchyard_fex_x64_state::gpr));
static_assert(sizeof(switchyard_fex_execution_domain) == SWITCHYARD_FEX_DOMAIN_SIZE_V1 &&
              offsetof(switchyard_fex_execution_domain, wake) == 16 &&
              offsetof(switchyard_fex_execution_domain, context) == 24);
static_assert(sizeof(FEXCore::Core::CPUState::mm) ==
              sizeof(switchyard_fex_x64_state::x87));
static_assert(sizeof(switchyard_fex_u128) == sizeof(__uint128_t));
static_assert(sizeof(FEXCore::Core::CPUState::XMMRegs::SSE::data) ==
              sizeof(switchyard_fex_x64_state::xmm));
static_assert(sizeof(FEXCore::Core::CPUState::avx_high) ==
              sizeof(switchyard_fex_x64_state::ymm_high));
// Export initializes members individually; no implicit padding may escape.
static_assert(sizeof(switchyard_fex_x64_state) ==
              sizeof(switchyard_fex_x64_state::size) + sizeof(switchyard_fex_x64_state::version) +
              sizeof(switchyard_fex_x64_state::flags) + sizeof(switchyard_fex_x64_state::reserved) +
              sizeof(switchyard_fex_x64_state::gpr) + sizeof(switchyard_fex_x64_state::rip) +
              sizeof(switchyard_fex_x64_state::rflags) + sizeof(switchyard_fex_x64_state::mxcsr) +
              sizeof(switchyard_fex_x64_state::fcw) + sizeof(switchyard_fex_x64_state::abridged_ftw) +
              sizeof(switchyard_fex_x64_state::reserved_fp) + sizeof(switchyard_fex_x64_state::segment) +
              sizeof(switchyard_fex_x64_state::reserved_segment) + sizeof(switchyard_fex_x64_state::segment_base) +
              sizeof(switchyard_fex_x64_state::xmm) + sizeof(switchyard_fex_x64_state::ymm_high) +
              sizeof(switchyard_fex_x64_state::x87));
static_assert(sizeof(switchyard_fex_executable_range) == SWITCHYARD_FEX_EXECUTABLE_RANGE_SIZE_V1);
static_assert(offsetof(switchyard_fex_executable_range, base) == 16);
static_assert(offsetof(switchyard_fex_executable_range, length) == 24);
static_assert(FEXCore::X86State::REG_RAX == 0 && FEXCore::X86State::REG_RCX == 1 &&
              FEXCore::X86State::REG_RDX == 2 && FEXCore::X86State::REG_RBX == 3 &&
              FEXCore::X86State::REG_RSP == 4 && FEXCore::X86State::REG_RBP == 5 &&
              FEXCore::X86State::REG_RSI == 6 && FEXCore::X86State::REG_RDI == 7 &&
              FEXCore::X86State::REG_R8 == 8 && FEXCore::X86State::REG_R9 == 9 &&
              FEXCore::X86State::REG_R10 == 10 && FEXCore::X86State::REG_R11 == 11 &&
              FEXCore::X86State::REG_R12 == 12 && FEXCore::X86State::REG_R13 == 13 &&
              FEXCore::X86State::REG_R14 == 14 && FEXCore::X86State::REG_R15 == 15);

std::mutex GlobalMutex;
struct switchyard_fex_process* ActiveProcess;
thread_local struct switchyard_fex_thread* ActiveExecutionThread;

void MessageHandler(LogMan::DebugLevels Level, const char* Message) {
  std::fprintf(stderr, "switchyard-fex %s %s\n", LogMan::DebugLevelStr(Level), Message);
}

void AssertHandler(const char* Message) {
  std::fprintf(stderr, "switchyard-fex assertion %s\n", Message);
  std::fflush(stderr);
}

void ReportInitializationFailure(const char* Stage, int Error = 0) {
  if (Error != 0) {
    std::fprintf(stderr, "switchyard-fex initialization stage %s failed: %s (%d)\n",
                 Stage, std::strerror(Error), Error);
  } else {
    std::fprintf(stderr, "switchyard-fex initialization stage %s failed\n", Stage);
  }
}

void* DarwinMmap(void* Address, size_t Length, int Protection, int Flags, int FD, off_t Offset) {
  if ((Protection & PROT_EXEC) != 0) {
    if ((Flags & MAP_FIXED) != 0) {
      errno = EINVAL;
      return MAP_FAILED;
    }
#ifdef MAP_FIXED_NOREPLACE
    if ((Flags & MAP_FIXED_NOREPLACE) != 0) {
      errno = EINVAL;
      return MAP_FAILED;
    }
#endif
    Flags |= MAP_JIT;
  }
  return ::mmap(Address, Length, Protection, Flags, FD, Offset);
}

int DarwinMunmap(void* Address, size_t Length) {
  return ::munmap(Address, Length);
}

std::optional<uint64_t> ReadUnsignedSysctl(const char* Name) {
  uint64_t Value {};
  size_t Size = sizeof(Value);
  if (::sysctlbyname(Name, &Value, &Size, nullptr, 0) != 0 ||
      (Size != sizeof(uint32_t) && Size != sizeof(uint64_t))) {
    return std::nullopt;
  }
  return Value;
}

bool ReadFeature(const char* Name) {
  const auto Value = ReadUnsignedSysctl(Name);
  return Value && *Value != 0;
}

std::optional<uint32_t> CacheLineEncoding() {
  const auto CacheLine = ReadUnsignedSysctl("hw.cachelinesize");
  if (!CacheLine || *CacheLine < 4 || (*CacheLine % 4) != 0) {
    return std::nullopt;
  }
  uint64_t Scaled = *CacheLine / 4;
  if ((Scaled & (Scaled - 1)) != 0) {
    return std::nullopt;
  }
  uint32_t Encoding {};
  while (Scaled > 1) {
    ++Encoding;
    Scaled >>= 1;
  }
  if (Encoding > 15) {
    return std::nullopt;
  }
  return Encoding;
}

std::optional<FEXCore::HostFeatures> DetectHostFeatures() {
  const auto CacheLine = CacheLineEncoding();
  if (!CacheLine) {
    return std::nullopt;
  }

  FEXCore::HostFeatures Features {};
  Features.DCacheLineLog2 = *CacheLine;
  Features.SupportsAES = ReadFeature("hw.optional.arm.FEAT_AES");
  Features.SupportsCRC = ReadFeature("hw.optional.arm.FEAT_CRC32");
  Features.SupportsSHA = ReadFeature("hw.optional.arm.FEAT_SHA1") &&
                         ReadFeature("hw.optional.arm.FEAT_SHA256");
  Features.SupportsAtomics = ReadFeature("hw.optional.arm.FEAT_LSE");
  Features.SupportsAFP = ReadFeature("hw.optional.arm.FEAT_AFP");
  Features.SupportsRCPC = ReadFeature("hw.optional.arm.FEAT_LRCPC");
  Features.SupportsTSOImm9 = ReadFeature("hw.optional.arm.FEAT_LRCPC2");
  Features.SupportsPMULL_128Bit = ReadFeature("hw.optional.arm.FEAT_PMULL");
  Features.SupportsCSSC = ReadFeature("hw.optional.arm.FEAT_CSSC");
  Features.SupportsFCMA = ReadFeature("hw.optional.arm.FEAT_FCMA");
  Features.SupportsFlagM = ReadFeature("hw.optional.arm.FEAT_FlagM");
  Features.SupportsFlagM2 = ReadFeature("hw.optional.arm.FEAT_FlagM2");
  Features.SupportsFRINTTS = ReadFeature("hw.optional.arm.FEAT_FRINTTS");
  Features.SupportsRPRES = ReadFeature("hw.optional.arm.FEAT_RPRES");
  Features.SupportsECV = ReadFeature("hw.optional.arm.FEAT_ECV");
  Features.SupportsWFXT = ReadFeature("hw.optional.arm.FEAT_WFxT");
  Features.SupportsMOPS = ReadFeature("hw.optional.arm.FEAT_MOPS");
  Features.SupportsFloatExceptions = ReadFeature("hw.optional.arm.FP_SyncExceptions");
  Features.SupportsAVX = true;
  Features.SupportsAES256 = Features.SupportsAES;
#if defined(__has_attribute)
# if __has_attribute(preserve_all)
  Features.SupportsPreserveAllABI = true;
# endif
#endif
  Features.HostType = FEXCore::HostFeatures::HostTypeEnum::Arm64ec;

  uint64_t LogicalCPUCount = ReadUnsignedSysctl("hw.logicalcpu").value_or(1);
  LogicalCPUCount = std::clamp<uint64_t>(LogicalCPUCount, 1, 256);
  Features.CPUMIDRs.resize(static_cast<size_t>(LogicalCPUCount));
  return Features;
}

void ConfigureLongMode(FEXCore::Core::CPUState& State) {
  State.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = State.private_gdt;
  State.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = State.private_gdt;

  auto& Code = State.private_gdt[FEXCore::Core::CPUState::DEFAULT_USER_CS];
  FEXCore::Core::CPUState::SetGDTBase(&Code, 0);
  FEXCore::Core::CPUState::SetGDTLimit(&Code, 0xF'FFFFU);
  Code.L = 1;
  Code.D = 0;
  State.cs_idx = FEXCore::Core::CPUState::DEFAULT_USER_CS << 3;
  State.cs_cached = 0;
}

class AdapterSyscallHandler final : public FEXCore::HLE::SyscallHandler {
public:
  switchyard_fex_executable_range_query ExecutableRangeQuery {};
  void* ExecutableRangeContext {};

  AdapterSyscallHandler() {
    OSABI = FEXCore::HLE::SyscallOSABI::OS_GENERIC_STOP;
  }

  uint64_t HandleSyscall(FEXCore::Core::CpuStateFrame*, FEXCore::HLE::SyscallArguments*) override {
    return 0;
  }

  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(
      FEXCore::Core::InternalThreadState*, uint64_t Address) override {
    switchyard_fex_executable_range Range {};
    Range.size = sizeof(Range);
    Range.version = SWITCHYARD_FEX_EXECUTABLE_RANGE_VERSION;
    if (!ExecutableRangeQuery ||
        ExecutableRangeQuery(ExecutableRangeContext, Address, &Range) != SWITCHYARD_FEX_OK ||
        Range.size != sizeof(Range) || Range.version != SWITCHYARD_FEX_EXECUTABLE_RANGE_VERSION ||
        Range.reserved || (Range.flags & ~SWITCHYARD_FEX_EXECUTABLE_RANGE_WRITABLE) ||
        !Range.length || Range.base > Address ||
        Range.base > std::numeric_limits<uint64_t>::max() - Range.length ||
        Address - Range.base >= Range.length) {
      return {Address, 0, false};
    }
    return {Range.base, Range.length, !!(Range.flags & SWITCHYARD_FEX_EXECUTABLE_RANGE_WRITABLE)};
  }

  std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(
      FEXCore::Core::InternalThreadState*, uint64_t) override {
    return std::nullopt;
  }
};

bool ValidConfig(const switchyard_fex_config* Config) {
  if (!Config || Config->size < sizeof(*Config) ||
      Config->abi_version != SWITCHYARD_FEX_ABI_VERSION ||
      (Config->flags & ~KnownConfigFlags) != 0 || Config->reserved != 0 ||
      Config->reserved2 != 0) {
    return false;
  }
  if (Config->low_va_shadow_base == 0 || Config->low_va_shadow_size == 0) {
    if (Config->low_va_shadow_base != 0 || Config->low_va_shadow_size != 0) {
      return false;
    }
  } else if (!std::has_single_bit(Config->low_va_shadow_base) ||
             Config->low_va_shadow_size != (UINT64_C(1) << 32) ||
             Config->low_va_shadow_base < Config->low_va_shadow_size ||
             Config->low_va_shadow_base >
               std::numeric_limits<uint64_t>::max() - Config->low_va_shadow_size) {
    return false;
  }

  if (Config->ec_code_bitmap == 0) {
    return Config->highest_user_address == 0 && Config->ec_page_shift == 0;
  }
  if ((Config->ec_code_bitmap & (alignof(uint64_t) - 1)) != 0 ||
      Config->highest_user_address == 0 ||
      Config->ec_page_shift < 12 || Config->ec_page_shift > 16) {
    return false;
  }
  const uint64_t LastWord =
    (Config->highest_user_address >> Config->ec_page_shift) >> 6;
  return LastWord <=
    (std::numeric_limits<uintptr_t>::max() - Config->ec_code_bitmap) /
      sizeof(uint64_t);
}

bool ValidExecution(const switchyard_fex_execution* Execution) {
  return Execution && Execution->size >= sizeof(*Execution) &&
         Execution->version == SWITCHYARD_FEX_EXECUTION_VERSION &&
         Execution->flags == 0 && Execution->reserved == 0 &&
         Execution->suspend_doorbell != 0 &&
         (Execution->suspend_doorbell & (alignof(uint32_t) - 1)) == 0;
}

bool ValidState(const switchyard_fex_x64_state* State) {
  return State && State->size >= sizeof(*State) &&
         State->version == SWITCHYARD_FEX_STATE_VERSION &&
         (State->flags & ~KnownStateFlags) == 0 && State->reserved == 0 &&
         State->reserved_fp == 0 && State->reserved_segment == 0 &&
         (State->mxcsr & ~ValidMXCSRMask) == 0;
}

void FillStop(switchyard_fex_stop* Stop, uint32_t Reason,
              const FEXCore::Core::InternalThreadState* Thread) {
  std::memset(Stop, 0, sizeof(*Stop));
  Stop->size = sizeof(*Stop);
  Stop->version = SWITCHYARD_FEX_STOP_VERSION;
  Stop->reason = Reason;
  if (!Thread) {
    return;
  }
  const auto& Frame = *Thread->CurrentFrame;
  Stop->signal = Frame.SynchronousFaultData.Signal;
  Stop->trap_number = Frame.SynchronousFaultData.TrapNo;
  Stop->signal_code = Frame.SynchronousFaultData.si_code;
  Stop->error_code = Frame.SynchronousFaultData.err_code;
  Stop->rip = Frame.State.rip;
}

} // namespace

struct switchyard_fex_process {
  std::mutex Mutex;
  std::vector<switchyard_fex_thread*> Threads;
  bool Destroying {};
  FEXCore::SignalDelegator Signals;
  AdapterSyscallHandler Syscalls;
  fextl::unique_ptr<FEXCore::Context::Context> Context;
  uint64_t LowAddressLimit {};
  uint64_t HostOffset {};
  void* BoundaryGuard {MAP_FAILED};
  size_t BoundaryGuardSize {};
  bool OwnsBoundaryGuard {};
  bool (*CustomX18Enabled)() {};
  void (*SetCustomX18Enabled)(bool) {};
  bool CustomDispatchEnabled {};
  bool SplitVectorState {};
  bool ExternalAdmission {};
};

struct switchyard_fex_thread {
  switchyard_fex_process* Process;
  FEXCore::Core::InternalThreadState* CoreThread;
  void* CallRetStackAllocation {MAP_FAILED};
  size_t CallRetStackAllocationSize {};
  switchyard_fex_admission Admission {};
  bool StateValid {};
  uint64_t ExecutionExpectedHLTRip {};
  void (*Wake)(void*) {};
  void* WakeContext {};
  // Unix-only attachment. No PE caller receives this storage or a writable
  // address of any TLS/admission authority. Legacy APIs retain their contract.
  pthread_t NativeGateOwner {};
  switchyard_fex_native_gate NativeGate {};
  // Positive geometry certificate, not a memory/CPU-state/mapping capability.
  // Only the authenticated host owner reads/writes it. Every call still borrows
  // fresh descriptors and payloads; no retained address is dereferenced.
  struct {
    uintptr_t Window {}, Output {}, Stop {}, Data {}, Core {}, Frame {}, Epoch {}, Doorbell {};
    uint64_t GS {};
  } NativeGeometry {};
  // EMPTY=0, CONSTRUCTING=1, PUBLISHED=2. Never recycle or overwrite a slot.
  // Abandonment during construction leaves a permanently cold (not locked)
  // cache. It cannot retain an execution/mapping lease or block a later call.
  std::atomic<uint32_t> NativeGeometryState {};
};
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(sizeof(switchyard_fex_native_gate) == SWITCHYARD_FEX_NATIVE_GATE_SIZE_V1 &&
              offsetof(switchyard_fex_native_gate, mapping_epoch) == 16);

// Private, single-consumption execution capability. It contains no owning C++
// object or stack address and may outlive a detached generated invocation.
// The attached thread pins core/context lifetime; mappings/code are pinned only
// while this activation is accounted. The C ABI exposes only its generation.
struct ExecutionActivation {
  switchyard_fex_thread* Owner {};
  uint64_t Generation {};
};
static_assert(std::is_trivially_copyable_v<ExecutionActivation> &&
              std::is_standard_layout_v<ExecutionActivation>);

// Short state operations never publish their token outside this C++ scope.
// Detached execution instead explicitly transfers a generation capability.
class StateAdmission {
  switchyard_fex_admission* Cell;
  uint64_t Token {};
  int Acquired {};
public:
  explicit StateAdmission(switchyard_fex_thread* Thread, bool Executing = false) : Cell {&Thread->Admission} {
    Acquired = switchyard_fex_admission_acquire(Cell, Executing, &Token);
  }
  StateAdmission(const StateAdmission&) = delete;
  StateAdmission& operator=(const StateAdmission&) = delete;
  ~StateAdmission() { if (Token) switchyard_fex_admission_release(Cell, Token); }
  explicit operator bool() const { return Token != 0; }
  int Acquisition() const { return Acquired; }
  uint64_t Generation() const { return Token >> 3; }
  void Detach() { Token = 0; } // Delete an owner, or transfer its exact execution capability.
};

// Process->Mutex pins membership for this entire scope. No waiter is needed:
// a busy closure is rolled back and the public API reports BUSY. The Wine
// provider already drains its executions before calling these mutation APIs.
class ClosedAdmissions {
  switchyard_fex_process* Process;
public:
  explicit ClosedAdmissions(switchyard_fex_process* Owner) : Process {Owner} {
    if (!Process->ExternalAdmission)
      for (auto* Thread : Process->Threads) switchyard_fex_admission_close(&Thread->Admission);
  }
  ClosedAdmissions(const ClosedAdmissions&) = delete;
  ClosedAdmissions& operator=(const ClosedAdmissions&) = delete;
  ~ClosedAdmissions() {
    if (!Process->ExternalAdmission)
      for (auto* Thread : Process->Threads) switchyard_fex_admission_reopen(&Thread->Admission);
  }
  bool Idle() const {
    for (const auto* Thread : Process->Threads) {
      const auto Word = switchyard_fex_admission_load(&Thread->Admission);
      if ((Word & SWITCHYARD_FEX_ADMISSION_ACTIVE) || !(Word & SWITCHYARD_FEX_ADMISSION_CLOSED)) return false;
    }
    return true;
  }
};

static bool InitializeCallRetStack(switchyard_fex_thread* Thread) {
  const long PageSizeValue = ::sysconf(_SC_PAGESIZE);
  const size_t UsableSize =
    FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;

  if (!Thread || !Thread->CoreThread || PageSizeValue <= 0) {
    return false;
  }
  const size_t PageSize = static_cast<size_t>(PageSizeValue);
  if (!std::has_single_bit(PageSize) || (UsableSize & (PageSize - 1)) != 0 ||
      PageSize > (std::numeric_limits<size_t>::max() - UsableSize) / 2) {
    return false;
  }
  const size_t AllocationSize = UsableSize + 2 * PageSize;
  void* const Allocation =
    ::mmap(nullptr, AllocationSize, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (Allocation == MAP_FAILED) {
    return false;
  }
  void* const Usable = static_cast<char*>(Allocation) + PageSize;
  if (::mprotect(Usable, UsableSize, PROT_READ | PROT_WRITE) != 0) {
    ::munmap(Allocation, AllocationSize);
    return false;
  }

  Thread->CallRetStackAllocation = Allocation;
  Thread->CallRetStackAllocationSize = AllocationSize;
  Thread->CoreThread->CallRetStackBase = Usable;
  Thread->CoreThread->CurrentFrame->State.callret_sp =
    reinterpret_cast<uintptr_t>(Usable) + UsableSize / 4;
  return true;
}

static void DestroyCallRetStack(switchyard_fex_thread* Thread) {
  if (!Thread || Thread->CallRetStackAllocation == MAP_FAILED) {
    return;
  }
  ::munmap(Thread->CallRetStackAllocation, Thread->CallRetStackAllocationSize);
  Thread->CallRetStackAllocation = MAP_FAILED;
  Thread->CallRetStackAllocationSize = 0;
}

// Keep fallback temporaries off the detected split-vector path's stack.
// The existing core helpers retain ownership of other vector layouts.
static void __attribute__((noinline)) ImportGenericVectorState(
    switchyard_fex_thread* Thread, const switchyard_fex_x64_state* Input) {
  __uint128_t XMM[16] {};
  __uint128_t YMMHigh[16] {};
  std::memcpy(XMM, Input->xmm, sizeof(XMM));
  std::memcpy(YMMHigh, Input->ymm_high, sizeof(YMMHigh));
  Thread->Process->Context->SetXMMRegistersFromState(
      Thread->CoreThread, XMM,
      (Input->flags & SWITCHYARD_FEX_STATE_YMM_HIGH_VALID) ? YMMHigh : nullptr);
}

static void __attribute__((noinline)) ExportGenericVectorState(
    switchyard_fex_thread* Thread, switchyard_fex_x64_state* Output) {
  __uint128_t XMM[16] {};
  __uint128_t YMMHigh[16] {};
  Thread->Process->Context->ReconstructXMMRegisters(Thread->CoreThread, XMM, YMMHigh);
  std::memcpy(Output->xmm, XMM, sizeof(XMM));
  std::memcpy(Output->ymm_high, YMMHigh, sizeof(YMMHigh));
}

static bool WindowOverlaps(uintptr_t Data, uintptr_t Object, size_t Size) {
  // Subtraction avoids constructing an overflowing end address.
  return Data >= Object ? Data - Object < Size :
         Object - Data < SWITCHYARD_FEX_REGISTER_WINDOW_DATA_SIZE_V1;
}

static switchyard_fex_result ValidateRegisterWindow(
    switchyard_fex_thread* Thread, const switchyard_fex_register_window* Window,
    bool Export) {
  if (!Thread || !Thread->CoreThread || !Window ||
      (reinterpret_cast<uintptr_t>(Window) & (alignof(switchyard_fex_register_window) - 1)) ||
      Window->size < sizeof(*Window)) return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  if (Window->version != SWITCHYARD_FEX_REGISTER_WINDOW_VERSION)
    return SWITCHYARD_FEX_ERROR_ABI_MISMATCH;
  if (Window->flags || Window->reserved || (Export && Window->gs_base) ||
      Window->data_size != SWITCHYARD_FEX_REGISTER_WINDOW_DATA_SIZE_V1 || !Window->data ||
      Window->data > std::numeric_limits<uintptr_t>::max() -
        (SWITCHYARD_FEX_REGISTER_WINDOW_DATA_SIZE_V1 - 1))
    return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  const auto Data = static_cast<uintptr_t>(Window->data);
  if (WindowOverlaps(Data, reinterpret_cast<uintptr_t>(Window), sizeof(*Window)) ||
      WindowOverlaps(Data, reinterpret_cast<uintptr_t>(Thread), sizeof(*Thread)) ||
      WindowOverlaps(Data, reinterpret_cast<uintptr_t>(Thread->Process), sizeof(*Thread->Process)) ||
      WindowOverlaps(Data, reinterpret_cast<uintptr_t>(Thread->CoreThread), sizeof(*Thread->CoreThread)) ||
      WindowOverlaps(Data, reinterpret_cast<uintptr_t>(Thread->CoreThread->CurrentFrame),
                     sizeof(*Thread->CoreThread->CurrentFrame)))
    return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  return SWITCHYARD_FEX_OK;
}

static constexpr size_t WindowGPROffsets[] = {
  SWITCHYARD_FEX_WINDOW_RAX, SWITCHYARD_FEX_WINDOW_RCX, SWITCHYARD_FEX_WINDOW_RDX,
  SWITCHYARD_FEX_WINDOW_RBX, SWITCHYARD_FEX_WINDOW_RSP, SWITCHYARD_FEX_WINDOW_RBP,
  SWITCHYARD_FEX_WINDOW_RSI, SWITCHYARD_FEX_WINDOW_RDI, SWITCHYARD_FEX_WINDOW_R8,
  SWITCHYARD_FEX_WINDOW_R9, SWITCHYARD_FEX_WINDOW_R10, SWITCHYARD_FEX_WINDOW_R11,
  SWITCHYARD_FEX_WINDOW_R12, SWITCHYARD_FEX_WINDOW_R13, SWITCHYARD_FEX_WINDOW_R14,
  SWITCHYARD_FEX_WINDOW_R15,
};

template<typename T> static T LoadWindow(const unsigned char* Data, size_t Offset) {
  T Value;
  std::memcpy(&Value, Data + Offset, sizeof(Value));
  return Value;
}

template<typename T> static void StoreWindow(unsigned char* Data, size_t Offset, T Value) {
  std::memcpy(Data + Offset, &Value, sizeof(Value));
}

static void __attribute__((noinline)) ImportGenericRegisterWindow(
    switchyard_fex_thread* Thread, const unsigned char* Data) {
  __uint128_t XMM[16] {};
  std::memcpy(XMM, Data + SWITCHYARD_FEX_WINDOW_XMM, sizeof(XMM));
  Thread->Process->Context->SetXMMRegistersFromState(Thread->CoreThread, XMM, nullptr);
}

static void __attribute__((noinline)) ExportGenericRegisterWindow(
    switchyard_fex_thread* Thread, unsigned char* Data) {
  __uint128_t XMM[16] {}, YMMHigh[16] {};
  // A non-null high buffer selects the actual converged representation too.
  Thread->Process->Context->ReconstructXMMRegisters(Thread->CoreThread, XMM, YMMHigh);
  std::memcpy(Data + SWITCHYARD_FEX_WINDOW_XMM, XMM, sizeof(XMM));
}

// Caller owns either short state admission or the stopped execution generation.
// No borrowed window or scope-bound activation escapes this conversion.
static inline __attribute__((always_inline)) switchyard_fex_result ExportRegisterWindowUnlocked(
    switchyard_fex_thread* Thread, const switchyard_fex_register_window* Window) {
  auto* Data = reinterpret_cast<unsigned char*>(static_cast<uintptr_t>(Window->data));
  uint32_t Flags;
  try {
    // Do not publish until every potentially throwing conversion has ended.
    Flags = Thread->Process->Context->ReconstructCompactedEFLAGS(Thread->CoreThread, false, nullptr, 0);
    if (!Thread->Process->SplitVectorState) ExportGenericRegisterWindow(Thread, Data);
  } catch (...) {
    return SWITCHYARD_FEX_ERROR_INTERNAL;
  }
  const auto& State = Thread->CoreThread->CurrentFrame->State;
  for (size_t Index = 0; Index < 16; ++Index)
    StoreWindow(Data, WindowGPROffsets[Index], State.gregs[Index]);
  StoreWindow(Data, SWITCHYARD_FEX_WINDOW_RIP, State.rip);
  StoreWindow(Data, SWITCHYARD_FEX_WINDOW_EFLAGS, static_cast<uint64_t>(Flags));
  StoreWindow(Data, SWITCHYARD_FEX_WINDOW_MXCSR, State.mxcsr);
  StoreWindow(Data, SWITCHYARD_FEX_WINDOW_RESERVED, uint32_t {0});
  if (Thread->Process->SplitVectorState)
    std::memcpy(Data + SWITCHYARD_FEX_WINDOW_XMM, State.xmm.sse.data, 256);
  return SWITCHYARD_FEX_OK;
}

static switchyard_fex_result ExportStateUnlocked(
    switchyard_fex_thread* Thread, switchyard_fex_x64_state* Output) {
  try {
    const auto& State = Thread->CoreThread->CurrentFrame->State;
    Output->size = sizeof(*Output);
    Output->version = SWITCHYARD_FEX_STATE_VERSION;
    Output->flags = SWITCHYARD_FEX_STATE_YMM_HIGH_VALID;
    Output->reserved = 0;
    std::memcpy(Output->gpr, State.gregs, sizeof(State.gregs));
    Output->rip = State.rip;
    Output->rflags = Thread->Process->Context->ReconstructCompactedEFLAGS(
        Thread->CoreThread, false, nullptr, 0);
    Output->mxcsr = State.mxcsr;
    Output->fcw = State.FCW;
    Output->abridged_ftw = State.AbridgedFTW;
    Output->reserved_fp = 0;
    Output->reserved_segment = 0;
    Output->segment[0] = State.es_idx;
    Output->segment[1] = State.cs_idx;
    Output->segment[2] = State.ss_idx;
    Output->segment[3] = State.ds_idx;
    Output->segment[4] = State.fs_idx;
    Output->segment[5] = State.gs_idx;
    Output->segment_base[0] = State.es_cached;
    Output->segment_base[1] = State.cs_cached;
    Output->segment_base[2] = State.ss_cached;
    Output->segment_base[3] = State.ds_cached;
    Output->segment_base[4] = State.fs_cached;
    Output->segment_base[5] = State.gs_cached;
    std::memcpy(Output->x87, State.mm, sizeof(State.mm));

    if (Thread->Process->SplitVectorState) {
      std::memcpy(Output->xmm, State.xmm.sse.data, sizeof(Output->xmm));
      std::memcpy(Output->ymm_high, State.avx_high, sizeof(Output->ymm_high));
    } else {
      ExportGenericVectorState(Thread, Output);
    }
  } catch (...) {
    // Do not leave a partial snapshot or caller bytes on an internal failure.
    std::memset(Output, 0, sizeof(*Output));
    Output->size = sizeof(*Output);
    Output->version = SWITCHYARD_FEX_STATE_VERSION;
    return SWITCHYARD_FEX_ERROR_INTERNAL;
  }
  return SWITCHYARD_FEX_OK;
}

static uint64_t ExecutionToken(uint64_t Generation) {
  return (Generation << 3) | SWITCHYARD_FEX_ADMISSION_ACTIVE | SWITCHYARD_FEX_ADMISSION_EXECUTING;
}

static bool OwnsExecution(const ExecutionActivation& Activation) {
  auto* const Thread = Activation.Owner;
  return Thread && ActiveExecutionThread == Thread && Activation.Generation &&
         Activation.Generation <= SWITCHYARD_FEX_ADMISSION_GENERATION_MAX &&
         (switchyard_fex_admission_load(&Thread->Admission) & ~SWITCHYARD_FEX_ADMISSION_CLOSED) ==
           ExecutionToken(Activation.Generation);
}

// Signal observers borrow the current execution; only reconstruction consumes
// the captured generation. TLS identifies the host owner, not a second lease.
static ExecutionActivation CurrentExecution(switchyard_fex_thread* Thread) {
  if (!Thread || ActiveExecutionThread != Thread) return {};
  const auto Word = switchyard_fex_admission_load(&Thread->Admission);
  if ((Word & (SWITCHYARD_FEX_ADMISSION_ACTIVE | SWITCHYARD_FEX_ADMISSION_EXECUTING)) !=
      (SWITCHYARD_FEX_ADMISSION_ACTIVE | SWITCHYARD_FEX_ADMISSION_EXECUTING)) return {};
  return {Thread, Word >> 3};
}

static bool FinishActiveExecution(const ExecutionActivation& Activation) {
  if (!OwnsExecution(Activation)) return false;
  auto* const Thread = Activation.Owner;
  ActiveExecutionThread = nullptr;
  Thread->CoreThread->CurrentFrame->ExternalSuspendDoorbell = 0;
  Thread->CoreThread->CurrentFrame->ExternalStop =
    FEXCore::Core::ExternalStopReason::None;
  uint64_t Released {};
  if (!switchyard_fex_admission_release_owned(&Thread->Admission, ExecutionToken(Activation.Generation), &Released))
    return false;
  if ((Released & SWITCHYARD_FEX_ADMISSION_CLOSED) && Thread->Wake) Thread->Wake(Thread->WakeContext);
  return true;
}

static inline __attribute__((always_inline)) switchyard_fex_result InitializeOwnedExecution(
    const ExecutionActivation& AcquiredActivation, const switchyard_fex_execution* Execution,
    ExecutionActivation& Activation) {
  auto* const Thread = AcquiredActivation.Owner;
  Thread->ExecutionExpectedHLTRip = Execution->expected_hlt_rip;
  ActiveExecutionThread = Thread;
  auto& Frame = *Thread->CoreThread->CurrentFrame;
  Frame.ExternalSuspendDoorbell = Execution->suspend_doorbell;
  Frame.ExternalStop = FEXCore::Core::ExternalStopReason::None;
  std::memset(&Frame.SynchronousFaultData, 0, sizeof(Frame.SynchronousFaultData));
  try {
    FEXCore::Allocator::InitializeThread();
  } catch (...) {
    FinishActiveExecution(AcquiredActivation);
    return SWITCHYARD_FEX_ERROR_INTERNAL;
  }
  Activation = AcquiredActivation;
  return SWITCHYARD_FEX_OK;
}

static switchyard_fex_result BeginExecution(
    switchyard_fex_thread* Thread, const switchyard_fex_execution* Execution,
    ExecutionActivation& Activation) {
  if (!Execution || Execution->size < sizeof(*Execution)) {
    return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  }
  if (Execution->version != SWITCHYARD_FEX_EXECUTION_VERSION) {
    return SWITCHYARD_FEX_ERROR_ABI_MISMATCH;
  }
  if (!ValidExecution(Execution) || !Thread || !Thread->CoreThread || !Thread->Process) {
    return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  }
  if (ActiveExecutionThread) return SWITCHYARD_FEX_ERROR_BUSY;
  uint64_t Token {};
  const int Acquired = switchyard_fex_admission_acquire(&Thread->Admission, true, &Token);
  if (!Acquired) return SWITCHYARD_FEX_ERROR_BUSY;
  if (Acquired < 0) return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  // Thread membership pins Process and Context lifetime. The process mutex is
  // only needed by membership changes and close-all mutation, not execution.
  if (!Thread->StateValid) {
    switchyard_fex_admission_release(&Thread->Admission, Token);
    return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  }
  const ExecutionActivation AcquiredActivation {Thread, Token >> 3};
  return InitializeOwnedExecution(AcquiredActivation, Execution, Activation);
}

static switchyard_fex_result CompleteExecution(
    const ExecutionActivation& Activation, switchyard_fex_stop* Stop,
    const switchyard_fex_register_window* Window = nullptr) {
  // Normalize a broken generated return before even accessing C++ TLS: Darwin
  // may implement thread-local access with a Mach-O runtime call. Opaque owner
  // lifetime is pinned by the caller throughout this private activation API.
  auto* const Thread = Activation.Owner;
  const auto* const Process = Thread ? Thread->Process : nullptr;
  const bool LeakedCustomMode = Process && Process->CustomX18Enabled && Process->CustomX18Enabled();
  if (LeakedCustomMode) Process->SetCustomX18Enabled(false);
  if (!Stop || Stop->size < sizeof(*Stop)) return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  if (Stop->version != SWITCHYARD_FEX_STOP_VERSION) return SWITCHYARD_FEX_ERROR_ABI_MISMATCH;
  // Reconstruction may already have consumed this activation. A stale token
  // must not release a newer execution or decrement accounting twice.
  if (!OwnsExecution(Activation)) return SWITCHYARD_FEX_ERROR_BUSY;
  const auto& Frame = *Thread->CoreThread->CurrentFrame;
  switchyard_fex_result Result = SWITCHYARD_FEX_OK;
  // Darwin owns the system x18 value; only its mode APIs are safe if an
  // incomplete generated exit leaked custom mode. Never restore old system x18.
  if (LeakedCustomMode) {
    FillStop(Stop, SWITCHYARD_FEX_STOP_INTERNAL, Thread->CoreThread);
    Result = SWITCHYARD_FEX_ERROR_INTERNAL;
  } else if (Frame.ExternalStop == FEXCore::Core::ExternalStopReason::InvalidCustomABI) {
    FillStop(Stop, SWITCHYARD_FEX_STOP_INTERNAL, Thread->CoreThread);
    Result = SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  } else if (Frame.ExternalStop == FEXCore::Core::ExternalStopReason::Transition) {
    FillStop(Stop, SWITCHYARD_FEX_STOP_EC_TRANSITION, Thread->CoreThread);
  } else if (Frame.ExternalStop == FEXCore::Core::ExternalStopReason::Syscall) {
    FillStop(Stop, SWITCHYARD_FEX_STOP_SYSCALL, Thread->CoreThread);
  } else if (Frame.ExternalStop == FEXCore::Core::ExternalStopReason::Suspend) {
    FillStop(Stop, SWITCHYARD_FEX_STOP_SUSPEND, Thread->CoreThread);
  } else if (Thread->ExecutionExpectedHLTRip && Frame.State.rip == Thread->ExecutionExpectedHLTRip &&
             Frame.SynchronousFaultData.Signal == FEXCore::Core::FAULT_SIGSEGV &&
             Frame.SynchronousFaultData.TrapNo == FEXCore::X86State::X86_TRAPNO_GP) {
    FillStop(Stop, SWITCHYARD_FEX_STOP_HLT, Thread->CoreThread);
  } else if (Frame.SynchronousFaultData.Signal == FEXCore::Core::FAULT_SIGILL &&
             Frame.SynchronousFaultData.TrapNo == FEXCore::X86State::X86_TRAPNO_UD) {
    FillStop(Stop, SWITCHYARD_FEX_STOP_INVALID_INSTRUCTION, Thread->CoreThread);
  } else if (Frame.SynchronousFaultData.Signal == FEXCore::Core::FAULT_SIGTRAP &&
             Frame.SynchronousFaultData.TrapNo == FEXCore::X86State::X86_TRAPNO_DB) {
    FillStop(Stop, SWITCHYARD_FEX_STOP_SINGLE_STEP, Thread->CoreThread);
  } else {
    FillStop(Stop, SWITCHYARD_FEX_STOP_GUEST_FAULT, Thread->CoreThread);
    Result = SWITCHYARD_FEX_ERROR_GUEST_FAULT;
  }
  if (Window && (Result == SWITCHYARD_FEX_OK || Result == SWITCHYARD_FEX_ERROR_GUEST_FAULT) &&
      Stop->reason != SWITCHYARD_FEX_STOP_SYSCALL) {
    const auto Exported = ExportRegisterWindowUnlocked(Thread, Window);
    if (Exported != SWITCHYARD_FEX_OK) {
      FillStop(Stop, SWITCHYARD_FEX_STOP_INTERNAL, Thread->CoreThread);
      Result = Exported;
    }
  }
  if (!FinishActiveExecution(Activation)) {
    FillStop(Stop, SWITCHYARD_FEX_STOP_INTERNAL, Thread->CoreThread);
    return SWITCHYARD_FEX_ERROR_INTERNAL;
  }
  return Result;
}

struct DetachedDispatch {
  ExecutionActivation Activation;
  FEXCore::Context::CustomDispatchEntry Entry {};
  FEXCore::Core::CpuStateFrame* Frame {};
};
static_assert(std::is_trivially_copyable_v<DetachedDispatch> &&
              std::is_standard_layout_v<DetachedDispatch>);

// Shares the ordinary execution accounting and cleanup path.
static switchyard_fex_result PrepareDetachedDispatch(
    switchyard_fex_thread* Thread, const switchyard_fex_execution* Execution,
    DetachedDispatch& Dispatch, uint64_t AuthenticatedX18 = 0) {
  ExecutionActivation Activation;
  const auto Result = BeginExecution(Thread, Execution, Activation);
  if (Result != SWITCHYARD_FEX_OK) return Result;
  if (AuthenticatedX18) Thread->CoreThread->CurrentFrame->CustomABIValue = AuthenticatedX18;
  const auto Entry = Thread->Process->Context->PrepareCustomDispatch(Thread->CoreThread);
  if (!Entry) {
    FinishActiveExecution(Activation);
    return SWITCHYARD_FEX_ERROR_UNSUPPORTED;
  }
  Dispatch = {Activation, Entry, Thread->CoreThread->CurrentFrame};
  return SWITCHYARD_FEX_OK;
}

static uint64_t GuestAddressForHostFault(const switchyard_fex_process* Process,
                                         uint64_t HostAddress) {
  if (HostAddress == 0 || Process->LowAddressLimit == 0) {
    return HostAddress;
  }
  if (HostAddress >= Process->HostOffset &&
      HostAddress - Process->HostOffset < Process->LowAddressLimit) {
    return HostAddress - Process->HostOffset;
  }
  if (Process->BoundaryGuard != MAP_FAILED &&
      HostAddress >= reinterpret_cast<uintptr_t>(Process->BoundaryGuard) &&
      HostAddress - reinterpret_cast<uintptr_t>(Process->BoundaryGuard) <
        Process->BoundaryGuardSize) {
    return Process->LowAddressLimit +
      (HostAddress - reinterpret_cast<uintptr_t>(Process->BoundaryGuard));
  }
  return HostAddress;
}

static bool IsProtectedBoundaryGuard(void* Address, size_t Size) {
  mach_vm_address_t RegionAddress = reinterpret_cast<uintptr_t>(Address);
  mach_vm_size_t RegionSize {};
  vm_region_basic_info_data_64_t Info {};
  mach_msg_type_number_t Count = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t Object = MACH_PORT_NULL;
  const auto Result = ::mach_vm_region(
    ::mach_task_self(), &RegionAddress, &RegionSize, VM_REGION_BASIC_INFO_64,
    reinterpret_cast<vm_region_info_t>(&Info), &Count, &Object);
  if (Object != MACH_PORT_NULL) {
    ::mach_port_deallocate(::mach_task_self(), Object);
  }
  if (Result != KERN_SUCCESS || RegionAddress > reinterpret_cast<uintptr_t>(Address)) {
    return false;
  }

  const auto Offset = reinterpret_cast<uintptr_t>(Address) - RegionAddress;
  return Offset <= RegionSize && Size <= RegionSize - Offset &&
    Info.protection == VM_PROT_NONE;
}

static bool AcquireBoundaryGuard(switchyard_fex_process* Process,
                                 void* Expected, size_t Size, int* Error) {
  auto* const Guard = ::mmap(Expected, Size, PROT_NONE,
                             MAP_PRIVATE | MAP_ANON, -1, 0);
  if (Guard == Expected) {
    Process->BoundaryGuard = Guard;
    Process->BoundaryGuardSize = Size;
    Process->OwnsBoundaryGuard = true;
    return true;
  }

  const int MapError = Guard == MAP_FAILED ? errno : EEXIST;
  if (Guard != MAP_FAILED) {
    ::munmap(Guard, Size);
  }
  if (!IsProtectedBoundaryGuard(Expected, Size)) {
    *Error = MapError;
    return false;
  }

  // Wine reserves this fail-closed page before any native allocation and
  // removes it from its allocator's free-area index.  Borrow that immutable
  // process-lifetime mapping instead of replacing or later unmapping it.
  Process->BoundaryGuard = Expected;
  Process->BoundaryGuardSize = Size;
  Process->OwnsBoundaryGuard = false;
  return true;
}

static void ReleaseBoundaryGuard(switchyard_fex_process* Process) {
  if (Process->BoundaryGuard != MAP_FAILED) {
    if (Process->OwnsBoundaryGuard) {
      ::munmap(Process->BoundaryGuard, Process->BoundaryGuardSize);
    }
    Process->BoundaryGuard = MAP_FAILED;
    Process->BoundaryGuardSize = 0;
    Process->OwnsBoundaryGuard = false;
  }
}

static bool RestoreJITExecutePermission() {
#if defined(__APPLE__) && defined(__aarch64__)
  if (!pthread_jit_write_protect_supported_np()) {
    return false;
  }
  pthread_jit_write_protect_np(1);
#endif

  return true;
}

extern "C" {

uint32_t switchyard_fex_abi_version(void) {
  return SWITCHYARD_FEX_ABI_VERSION;
}

const char* switchyard_fex_provider_abi_identity(void) {
  return SWITCHYARD_FEX_PROVIDER_ABI_IDENTITY;
}

const char* switchyard_fex_upstream_revision(void) {
  return SWITCHYARD_FEX_UPSTREAM_REVISION;
}

const char* switchyard_fex_result_string(switchyard_fex_result Result) {
  switch (Result) {
  case SWITCHYARD_FEX_OK: return "success";
  case SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT: return "invalid argument";
  case SWITCHYARD_FEX_ERROR_ABI_MISMATCH: return "ABI mismatch";
  case SWITCHYARD_FEX_ERROR_UNSUPPORTED: return "unsupported operation";
  case SWITCHYARD_FEX_ERROR_NO_MEMORY: return "out of memory";
  case SWITCHYARD_FEX_ERROR_BUSY: return "object is busy";
  case SWITCHYARD_FEX_ERROR_INITIALIZATION: return "initialization failed";
  case SWITCHYARD_FEX_ERROR_GUEST_FAULT: return "guest fault";
  case SWITCHYARD_FEX_ERROR_INTERNAL: return "internal error";
  default: return "unknown result";
  }
}

switchyard_fex_result switchyard_fex_process_create(
    const switchyard_fex_config* Config, switchyard_fex_process** OutProcess) {
  if (!OutProcess) {
    return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  }
  *OutProcess = nullptr;
  if (!Config || Config->size < sizeof(*Config)) {
    return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  }
  if (Config->abi_version != SWITCHYARD_FEX_ABI_VERSION) {
    return SWITCHYARD_FEX_ERROR_ABI_MISMATCH;
  }
  if (!ValidConfig(Config)) {
    return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  }
  if (!pthread_jit_write_protect_supported_np()) {
    return SWITCHYARD_FEX_ERROR_UNSUPPORTED;
  }

  std::lock_guard GlobalLock(GlobalMutex);
  if (ActiveProcess) {
    return SWITCHYARD_FEX_ERROR_BUSY;
  }

  auto* Process = new (std::nothrow) switchyard_fex_process {};
  if (!Process) {
    return SWITCHYARD_FEX_ERROR_NO_MEMORY;
  }

  if (Config->low_va_shadow_size != 0) {
    const long PageSize = ::sysconf(_SC_PAGESIZE);
    const uint64_t BoundaryAddress =
      Config->low_va_shadow_base + Config->low_va_shadow_size;
    if (PageSize <= 0 ||
        (static_cast<unsigned long>(PageSize) &
         (static_cast<unsigned long>(PageSize) - 1)) != 0 ||
        BoundaryAddress >
          std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(PageSize)) {
      ReportInitializationFailure("low-shadow-boundary-page-size",
                                  PageSize <= 0 ? errno : 0);
      delete Process;
      return SWITCHYARD_FEX_ERROR_INITIALIZATION;
    }
    auto* const ExpectedGuard = reinterpret_cast<void*>(
      static_cast<uintptr_t>(BoundaryAddress));
    int GuardError {};
    if (!AcquireBoundaryGuard(Process, ExpectedGuard,
                              static_cast<size_t>(PageSize), &GuardError)) {
      ReportInitializationFailure("low-shadow-boundary-guard", GuardError);
      delete Process;
      return SWITCHYARD_FEX_ERROR_INITIALIZATION;
    }
  }

  bool ConfigInitialized {};
  bool HandlersInstalled {};
  try {
    const auto Features = DetectHostFeatures();
    if (!Features || !Features->SupportsAtomics) {
      ReleaseBoundaryGuard(Process);
      delete Process;
      return SWITCHYARD_FEX_ERROR_UNSUPPORTED;
    }
#ifdef SWITCHYARD_FEX_HAS_CUSTOM_X18_API
    // Resolve availability in system mode, before publishing the process. The
    // return guard must not call an availability helper from a broken JIT exit.
    if (__builtin_available(macOS 26.4, *)) {
      Process->CustomX18Enabled = os_custom_x18_abi_enabled;
      Process->SetCustomX18Enabled = os_set_custom_x18_abi_enabled;
    }
#endif
    if ((Config->flags & SWITCHYARD_FEX_CONFIG_CUSTOM_DISPATCH) &&
        !Process->SetCustomX18Enabled) {
      ReleaseBoundaryGuard(Process);
      delete Process;
      return SWITCHYARD_FEX_ERROR_UNSUPPORTED;
    }

    LogMan::Throw::InstallHandler(AssertHandler);
    LogMan::Msg::InstallHandler(MessageHandler);
    HandlersInstalled = true;
    FEXCore::Config::Initialize();
    ConfigInitialized = true;
    FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
    FEXCore::Config::Set(FEXCore::Config::CONFIG_MULTIBLOCK,
                         (Config->flags & SWITCHYARD_FEX_CONFIG_MULTIBLOCK) ? "1" : "0");
    FEXCore::Config::Set(FEXCore::Config::CONFIG_ENABLECODECACHINGWIP, "0");
    FEXCore::Config::Set(FEXCore::Config::CONFIG_SMCCHECKS, "0");
    FEXCore::Config::Set(FEXCore::Config::CONFIG_DISABLETELEMETRY, "1");

    FEXCore::Allocator::mmap = DarwinMmap;
    FEXCore::Allocator::munmap = DarwinMunmap;
    FEXCore::Allocator::InitializeThread();

    Process->Context = FEXCore::Context::Context::CreateNewContext(*Features);
    if (!Process->Context) {
      throw std::bad_alloc {};
    }
    // This adapter configures only 64-bit guests. Use the exact features passed
    // to the core: split AVX lanes already match the C ABI's byte layout. Keep
    // the core conversion for any other layout, without assuming an M-series
    // generation or casting the C ABI's 8-byte-aligned lanes to uint128_t*.
    Process->SplitVectorState = Features->SupportsAVX && !Features->SupportsSVE256;
    Process->ExternalAdmission = Config->flags & SWITCHYARD_FEX_CONFIG_EXTERNAL_ADMISSION;
    Process->Context->SetSignalDelegator(&Process->Signals);
    Process->Context->SetSyscallHandler(&Process->Syscalls);
    if (!Process->Context->SetGuestMemoryMapping(Config->low_va_shadow_size,
                                                 Config->low_va_shadow_base)) {
      throw InitializationFailure {"guest-memory-mapping"};
    }
    if (!Process->Context->SetExternalExecutionStops(
          reinterpret_cast<const uint64_t*>(
            static_cast<uintptr_t>(Config->ec_code_bitmap)),
          Config->highest_user_address, Config->ec_page_shift)) {
      throw InitializationFailure {"external-execution-stops"};
    }
    Process->LowAddressLimit = Config->low_va_shadow_size;
    Process->HostOffset = Config->low_va_shadow_base;
    if (Config->flags & SWITCHYARD_FEX_CONFIG_CUSTOM_DISPATCH) {
      if (!Process->Context->SetCustomABITransition(Process->SetCustomX18Enabled)) {
        throw InitializationFailure {"custom-dispatch"};
      }
      Process->CustomDispatchEnabled = true;
    }
    Process->Context->EnableExitOnHLT();
    if (!Process->Context->InitCore()) {
      Process->Context.reset();
      FEXCore::Allocator::mmap = ::mmap;
      FEXCore::Allocator::munmap = ::munmap;
      FEXCore::Config::Shutdown();
      LogMan::Throw::UnInstallHandler();
      LogMan::Msg::UnInstallHandler();
      ReleaseBoundaryGuard(Process);
      delete Process;
      ReportInitializationFailure("core-initialization");
      return SWITCHYARD_FEX_ERROR_INITIALIZATION;
    }
  } catch (const InitializationFailure& Failure) {
    Process->Context.reset();
    FEXCore::Allocator::mmap = ::mmap;
    FEXCore::Allocator::munmap = ::munmap;
    if (ConfigInitialized) FEXCore::Config::Shutdown();
    if (HandlersInstalled) {
      LogMan::Throw::UnInstallHandler();
      LogMan::Msg::UnInstallHandler();
    }
    ReleaseBoundaryGuard(Process);
    delete Process;
    ReportInitializationFailure(Failure.Stage);
    return SWITCHYARD_FEX_ERROR_INITIALIZATION;
  } catch (const std::bad_alloc&) {
    Process->Context.reset();
    FEXCore::Allocator::mmap = ::mmap;
    FEXCore::Allocator::munmap = ::munmap;
    if (ConfigInitialized) FEXCore::Config::Shutdown();
    if (HandlersInstalled) {
      LogMan::Throw::UnInstallHandler();
      LogMan::Msg::UnInstallHandler();
    }
    ReleaseBoundaryGuard(Process);
    delete Process;
    return SWITCHYARD_FEX_ERROR_NO_MEMORY;
  } catch (...) {
    Process->Context.reset();
    FEXCore::Allocator::mmap = ::mmap;
    FEXCore::Allocator::munmap = ::munmap;
    if (ConfigInitialized) FEXCore::Config::Shutdown();
    if (HandlersInstalled) {
      LogMan::Throw::UnInstallHandler();
      LogMan::Msg::UnInstallHandler();
    }
    ReleaseBoundaryGuard(Process);
    delete Process;
    return SWITCHYARD_FEX_ERROR_INTERNAL;
  }

  ActiveProcess = Process;
  *OutProcess = Process;
  return SWITCHYARD_FEX_OK;
}

switchyard_fex_result switchyard_fex_process_set_executable_range_query(
    switchyard_fex_process* Process, switchyard_fex_executable_range_query Query, void* Context) {
  if (!Process || !Query) {
    return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  }
  std::lock_guard Lock(Process->Mutex);
  if (Process->Destroying || !Process->Threads.empty() || Process->Syscalls.ExecutableRangeQuery) {
    return SWITCHYARD_FEX_ERROR_BUSY;
  }
  Process->Syscalls.ExecutableRangeContext = Context;
  Process->Syscalls.ExecutableRangeQuery = Query;
  return SWITCHYARD_FEX_OK;
}

switchyard_fex_result switchyard_fex_process_destroy(switchyard_fex_process* Process) {
  if (!Process) {
    return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  }
  std::lock_guard GlobalLock(GlobalMutex);
  if (ActiveProcess != Process) {
    return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  }
  {
    std::lock_guard Lock(Process->Mutex);
    if (Process->Destroying || !Process->Threads.empty()) {
      return SWITCHYARD_FEX_ERROR_BUSY;
    }
    Process->Destroying = true;
  }
  Process->Context.reset();
  FEXCore::Allocator::mmap = ::mmap;
  FEXCore::Allocator::munmap = ::munmap;
  FEXCore::Config::Shutdown();
  LogMan::Throw::UnInstallHandler();
  LogMan::Msg::UnInstallHandler();
  ReleaseBoundaryGuard(Process);
  ActiveProcess = nullptr;
  delete Process;
  return SWITCHYARD_FEX_OK;
}

static switchyard_fex_result CreateAdapterThread(
    switchyard_fex_process* Process, const switchyard_fex_execution_domain* Domain,
    switchyard_fex_thread** OutThread, switchyard_fex_admission** OutAdmission) {
  if (!Process || !OutThread) {
    return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  }
  *OutThread = nullptr;
  if (OutAdmission) *OutAdmission = nullptr;
  if (bool(Domain) != Process->ExternalAdmission) return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  if (Domain) {
    if (!OutAdmission || Domain->size < sizeof(*Domain) || Domain->flags || Domain->reserved || !Domain->wake)
      return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
    if (Domain->version != SWITCHYARD_FEX_DOMAIN_VERSION) return SWITCHYARD_FEX_ERROR_ABI_MISMATCH;
  }
  auto* Thread = new (std::nothrow) switchyard_fex_thread {Process, nullptr};
  if (!Thread) {
    return SWITCHYARD_FEX_ERROR_NO_MEMORY;
  }
  Thread->NativeGateOwner = pthread_self();
  if (Domain) {
    Thread->Wake = Domain->wake;
    Thread->WakeContext = Domain->context;
  }

  std::lock_guard Lock(Process->Mutex);
  if (Process->Destroying) {
    delete Thread;
    return SWITCHYARD_FEX_ERROR_BUSY;
  }
  try {
    Process->Threads.reserve(Process->Threads.size() + 1);
    FEXCore::Allocator::InitializeThread();
    Thread->CoreThread = Process->Context->CreateThread();
    if (!Thread->CoreThread) {
      delete Thread;
      return SWITCHYARD_FEX_ERROR_NO_MEMORY;
    }
    if (!InitializeCallRetStack(Thread)) {
      Process->Context->DestroyThread(Thread->CoreThread);
      delete Thread;
      return SWITCHYARD_FEX_ERROR_NO_MEMORY;
    }
    ConfigureLongMode(Thread->CoreThread->CurrentFrame->State);
    Process->Threads.push_back(Thread);
  } catch (const std::bad_alloc&) {
    DestroyCallRetStack(Thread);
    if (Thread->CoreThread) Process->Context->DestroyThread(Thread->CoreThread);
    delete Thread;
    return SWITCHYARD_FEX_ERROR_NO_MEMORY;
  } catch (...) {
    DestroyCallRetStack(Thread);
    if (Thread->CoreThread) Process->Context->DestroyThread(Thread->CoreThread);
    delete Thread;
    return SWITCHYARD_FEX_ERROR_INTERNAL;
  }
  *OutThread = Thread;
  if (OutAdmission) *OutAdmission = &Thread->Admission;
  return SWITCHYARD_FEX_OK;
}

switchyard_fex_result switchyard_fex_thread_create(
    switchyard_fex_process* Process, switchyard_fex_thread** OutThread) {
  return CreateAdapterThread(Process, nullptr, OutThread, nullptr);
}

switchyard_fex_result switchyard_fex_thread_create_with_domain(
    switchyard_fex_process* Process, const switchyard_fex_execution_domain* Domain,
    switchyard_fex_thread** OutThread, switchyard_fex_admission** OutAdmission) {
  if (!Domain || !OutAdmission || static_cast<void*>(OutThread) == static_cast<void*>(OutAdmission))
    return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  return CreateAdapterThread(Process, Domain, OutThread, OutAdmission);
}

switchyard_fex_result switchyard_fex_thread_destroy(switchyard_fex_thread* Thread) {
  if (!Thread || !Thread->Process || !Thread->CoreThread) {
    return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  }
  auto* Process = Thread->Process;
  std::lock_guard Lock(Process->Mutex);
  const auto It = std::find(Process->Threads.begin(), Process->Threads.end(), Thread);
  if (Process->Destroying || It == Process->Threads.end()) {
    return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  }
  // A different thread's process-wide invalidation is permitted during detach.
  // Wait for its closure before admission: otherwise a transient CLOSED would
  // reject teardown after Wine had already removed the binding from its list.
  std::optional<StateAdmission> Admission;
  if (Process->ExternalAdmission) {
    const auto Word = switchyard_fex_admission_load(&Thread->Admission);
    if (Word & SWITCHYARD_FEX_ADMISSION_ACTIVE) return SWITCHYARD_FEX_ERROR_BUSY;
    if (!(Word & SWITCHYARD_FEX_ADMISSION_CLOSED)) return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  } else {
    Admission.emplace(Thread);
    if (!*Admission) return SWITCHYARD_FEX_ERROR_BUSY;
  }
  try {
    DestroyCallRetStack(Thread);
    Process->Context->DestroyThread(Thread->CoreThread);
  } catch (...) {
    return SWITCHYARD_FEX_ERROR_INTERNAL;
  }
  Process->Threads.erase(It);
  Thread->CoreThread = nullptr;
  Thread->Process = nullptr;
  if (Admission) Admission->Detach();
  delete Thread;
  return SWITCHYARD_FEX_OK;
}

switchyard_fex_result switchyard_fex_thread_import_state(
    switchyard_fex_thread* Thread, const switchyard_fex_x64_state* Input) {
  if (!Thread || !Thread->CoreThread || !Input || Input->size < sizeof(*Input)) {
    return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  }
  if (Input->version != SWITCHYARD_FEX_STATE_VERSION) {
    return SWITCHYARD_FEX_ERROR_ABI_MISMATCH;
  }
  if (!ValidState(Input)) {
    return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  }
  if (Input->rflags > std::numeric_limits<uint32_t>::max()) {
    return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  }
  StateAdmission Admission {Thread};
  if (!Admission) {
    return SWITCHYARD_FEX_ERROR_BUSY;
  }
  for (size_t Index = 0; Index < 4; ++Index) {
    if (Input->segment_base[Index] > std::numeric_limits<uint32_t>::max()) {
      return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
    }
  }

  auto& State = Thread->CoreThread->CurrentFrame->State;
  try {
    std::memcpy(State.gregs, Input->gpr, sizeof(State.gregs));
    State.rip = Input->rip;
    State.mxcsr = Input->mxcsr;
    State.FCW = Input->fcw;
    State.AbridgedFTW = Input->abridged_ftw;
    State.es_idx = Input->segment[0];
    State.cs_idx = Input->segment[1];
    State.ss_idx = Input->segment[2];
    State.ds_idx = Input->segment[3];
    State.fs_idx = Input->segment[4];
    State.gs_idx = Input->segment[5];
    State.es_cached = static_cast<uint32_t>(Input->segment_base[0]);
    State.cs_cached = static_cast<uint32_t>(Input->segment_base[1]);
    State.ss_cached = static_cast<uint32_t>(Input->segment_base[2]);
    State.ds_cached = static_cast<uint32_t>(Input->segment_base[3]);
    State.fs_cached = Input->segment_base[4];
    State.gs_cached = Input->segment_base[5];
    std::memcpy(State.mm, Input->x87, sizeof(State.mm));
    Thread->Process->Context->SetFlagsFromCompactedEFLAGS(
        Thread->CoreThread, static_cast<uint32_t>(Input->rflags));

    if (Thread->Process->SplitVectorState) {
      std::memcpy(State.xmm.sse.data, Input->xmm, sizeof(Input->xmm));
      // A missing high half means preserve it, not zero it. This matches the
      // core helper's null YMM_High contract at every native boundary.
      if (Input->flags & SWITCHYARD_FEX_STATE_YMM_HIGH_VALID)
        std::memcpy(State.avx_high, Input->ymm_high, sizeof(Input->ymm_high));
    } else {
      ImportGenericVectorState(Thread, Input);
    }
    Thread->StateValid = true;
  } catch (const std::bad_alloc&) {
    return SWITCHYARD_FEX_ERROR_NO_MEMORY;
  } catch (...) {
    return SWITCHYARD_FEX_ERROR_INTERNAL;
  }
  return SWITCHYARD_FEX_OK;
}

switchyard_fex_result switchyard_fex_thread_export_state(
    switchyard_fex_thread* Thread, switchyard_fex_x64_state* Output) {
  if (!Thread || !Thread->CoreThread || !Output || Output->size < sizeof(*Output)) {
    return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  }
  if (Output->version != SWITCHYARD_FEX_STATE_VERSION) {
    return SWITCHYARD_FEX_ERROR_ABI_MISMATCH;
  }
  StateAdmission Admission {Thread};
  if (!Admission) {
    return SWITCHYARD_FEX_ERROR_BUSY;
  }
  if (!Thread->StateValid) {
    return SWITCHYARD_FEX_ERROR_BUSY;
  }

  return ExportStateUnlocked(Thread, Output);
}

static inline __attribute__((always_inline)) switchyard_fex_result ImportRegisterWindowUnlocked(
    switchyard_fex_thread* Thread, const switchyard_fex_register_window* Window) {
  const auto* Data = reinterpret_cast<const unsigned char*>(static_cast<uintptr_t>(Window->data));
  const auto Flags = LoadWindow<uint64_t>(Data, SWITCHYARD_FEX_WINDOW_EFLAGS);
  const auto MXCSR = LoadWindow<uint32_t>(Data, SWITCHYARD_FEX_WINDOW_MXCSR);
  if (Flags > std::numeric_limits<uint32_t>::max() || (MXCSR & ~ValidMXCSRMask))
    return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  auto& State = Thread->CoreThread->CurrentFrame->State;
  try {
    for (size_t Index = 0; Index < 16; ++Index)
      State.gregs[Index] = LoadWindow<uint64_t>(Data, WindowGPROffsets[Index]);
    State.rip = LoadWindow<uint64_t>(Data, SWITCHYARD_FEX_WINDOW_RIP);
    State.mxcsr = MXCSR;
    State.FCW = 0x037f;
    State.AbridgedFTW = 0;
    State.es_idx = State.ss_idx = State.ds_idx = State.fs_idx = State.gs_idx = 0;
    State.cs_idx = 0x30;
    State.es_cached = State.cs_cached = State.ss_cached = State.ds_cached = 0;
    State.fs_cached = 0;
    State.gs_cached = Window->gs_base;
    std::memset(State.mm, 0, sizeof(State.mm));
    Thread->Process->Context->SetFlagsFromCompactedEFLAGS(Thread->CoreThread, static_cast<uint32_t>(Flags));
    if (Thread->Process->SplitVectorState)
      std::memcpy(State.xmm.sse.data, Data + SWITCHYARD_FEX_WINDOW_XMM, 256);
    else ImportGenericRegisterWindow(Thread, Data);
    Thread->StateValid = true;
  } catch (const std::bad_alloc&) {
    Thread->StateValid = false;
    return SWITCHYARD_FEX_ERROR_NO_MEMORY;
  } catch (...) {
    Thread->StateValid = false;
    return SWITCHYARD_FEX_ERROR_INTERNAL;
  }
  return SWITCHYARD_FEX_OK;
}

switchyard_fex_result switchyard_fex_thread_import_register_window(
    switchyard_fex_thread* Thread, const switchyard_fex_register_window* Window) {
  const auto Result = ValidateRegisterWindow(Thread, Window, false);
  if (Result != SWITCHYARD_FEX_OK) return Result;
  StateAdmission Admission {Thread};
  if (!Admission) return SWITCHYARD_FEX_ERROR_BUSY;
  return ImportRegisterWindowUnlocked(Thread, Window);
}

// Private experiment: one admission covers native-entry import and execution
// reservation. No C++ activation or borrowed window survives this function.
// Pre-publication failures are short state operations: release without wake.
// As with prepare_execution, the external embedding holds its mutation mutex
// here, then unlocks BEFORE execution/completion; correct external closure
// cannot race this pre-publication interval.
switchyard_fex_result switchyard_fex_experiment_prepare_native_window(
    switchyard_fex_thread* Thread, const switchyard_fex_register_window* Window,
    const switchyard_fex_execution* Execution, uint64_t* Generation) {
  if (!Generation) return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  const auto ValidWindow = ValidateRegisterWindow(Thread, Window, false);
  if (ValidWindow != SWITCHYARD_FEX_OK) return ValidWindow;
  if (!Execution || Execution->size < sizeof(*Execution))
    return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  if (Execution->version != SWITCHYARD_FEX_EXECUTION_VERSION)
    return SWITCHYARD_FEX_ERROR_ABI_MISMATCH;
  if (!ValidExecution(Execution)) return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  if (ActiveExecutionThread) return SWITCHYARD_FEX_ERROR_BUSY;
  StateAdmission Admission {Thread, true};
  if (!Admission) return Admission.Acquisition() < 0 ? SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT : SWITCHYARD_FEX_ERROR_BUSY;
  const auto Imported = ImportRegisterWindowUnlocked(Thread, Window);
  if (Imported != SWITCHYARD_FEX_OK) return Imported;
  const ExecutionActivation Acquired {Thread, Admission.Generation()};
  // Transfer exact-generation ownership, never hold a scope-bound state lease
  // across the execution or release it a second time in this destructor.
  Admission.Detach();
  ExecutionActivation Activation;
  const auto Prepared = InitializeOwnedExecution(Acquired, Execution, Activation);
  if (Prepared == SWITCHYARD_FEX_OK) *Generation = Activation.Generation;
  return Prepared;
}

switchyard_fex_result switchyard_fex_thread_export_register_window(
    switchyard_fex_thread* Thread, const switchyard_fex_register_window* Window) {
  const auto Result = ValidateRegisterWindow(Thread, Window, true);
  if (Result != SWITCHYARD_FEX_OK) return Result;
  StateAdmission Admission {Thread};
  if (!Admission || !Thread->StateValid) return SWITCHYARD_FEX_ERROR_BUSY;
  return ExportRegisterWindowUnlocked(Thread, Window);
}

switchyard_fex_result switchyard_fex_thread_prepare_execution(
    switchyard_fex_thread* Thread, const switchyard_fex_execution* Execution, uint64_t* Generation) {
  if (!Generation) return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  ExecutionActivation Activation;
  const auto Result = BeginExecution(Thread, Execution, Activation);
  if (Result == SWITCHYARD_FEX_OK) *Generation = Activation.Generation;
  return Result;
}

switchyard_fex_result switchyard_fex_thread_execute_prepared(
    switchyard_fex_thread* Thread, uint64_t Generation, switchyard_fex_stop* Stop) {
  if (!Stop || Stop->size < sizeof(*Stop)) {
    return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  }
  if (Stop->version != SWITCHYARD_FEX_STOP_VERSION) {
    return SWITCHYARD_FEX_ERROR_ABI_MISMATCH;
  }
  const ExecutionActivation Activation {Thread, Generation};
  if (!OwnsExecution(Activation)) return SWITCHYARD_FEX_ERROR_BUSY;
  try {
    Thread->Process->Context->ExecuteThread(Thread->CoreThread);
  } catch (...) {
    FillStop(Stop, SWITCHYARD_FEX_STOP_INTERNAL, Thread->CoreThread);
    FinishActiveExecution(Activation);
    return SWITCHYARD_FEX_ERROR_INTERNAL;
  }
  return CompleteExecution(Activation, Stop);
}

static bool ObjectOverlap(uintptr_t Left, size_t LeftSize, uintptr_t Right, size_t RightSize) {
  return Left >= Right ? Left - Right < RightSize : Right - Left < LeftSize;
}

static switchyard_fex_result ValidateCompletionWindow(
    switchyard_fex_thread* Thread, switchyard_fex_stop* Stop,
    const switchyard_fex_register_window* Window) {
  if (!Stop || (reinterpret_cast<uintptr_t>(Stop) & (alignof(switchyard_fex_stop) - 1)) ||
      Stop->size < sizeof(*Stop)) return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  if (Stop->version != SWITCHYARD_FEX_STOP_VERSION) return SWITCHYARD_FEX_ERROR_ABI_MISMATCH;
  const auto Result = ValidateRegisterWindow(Thread, Window, true);
  if (Result != SWITCHYARD_FEX_OK) return Result;
  const auto Pointer = reinterpret_cast<uintptr_t>(Stop);
  if (WindowOverlaps(static_cast<uintptr_t>(Window->data), Pointer, sizeof(*Stop)) ||
      ObjectOverlap(Pointer, sizeof(*Stop), reinterpret_cast<uintptr_t>(Window), sizeof(*Window)) ||
      ObjectOverlap(Pointer, sizeof(*Stop), reinterpret_cast<uintptr_t>(Thread), sizeof(*Thread)) ||
      ObjectOverlap(Pointer, sizeof(*Stop), reinterpret_cast<uintptr_t>(Thread->Process), sizeof(*Thread->Process)) ||
      ObjectOverlap(Pointer, sizeof(*Stop), reinterpret_cast<uintptr_t>(Thread->CoreThread), sizeof(*Thread->CoreThread)) ||
      ObjectOverlap(Pointer, sizeof(*Stop), reinterpret_cast<uintptr_t>(Thread->CoreThread->CurrentFrame),
                    sizeof(*Thread->CoreThread->CurrentFrame)))
    return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  return SWITCHYARD_FEX_OK;
}

static switchyard_fex_result ValidateNativeGateGeometryUncached(
    switchyard_fex_thread* Thread, const switchyard_fex_register_window* Window,
    switchyard_fex_stop* Stop, switchyard_fex_register_window& Output) {
  const auto Valid = ValidateRegisterWindow(Thread, Window, false);
  if (Valid != SWITCHYARD_FEX_OK) return Valid;
  const auto& Gate = Thread->NativeGate;
  if (Window->gs_base != Gate.gs_base) return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  Output = *Window;
  Output.gs_base = 0;
  if (!Stop || (reinterpret_cast<uintptr_t>(Stop) & (alignof(switchyard_fex_stop) - 1)) ||
      ObjectOverlap(reinterpret_cast<uintptr_t>(Stop), sizeof(*Stop),
                    reinterpret_cast<uintptr_t>(Window), sizeof(*Window)))
    return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  const auto Completion = ValidateCompletionWindow(Thread, Stop, &Output);
  if (Completion != SWITCHYARD_FEX_OK) return Completion;
  const auto Data = static_cast<uintptr_t>(Window->data);
  const auto StopPointer = reinterpret_cast<uintptr_t>(Stop);
  const auto EpochPointer = static_cast<uintptr_t>(Gate.mapping_epoch);
  const auto DoorbellPointer = static_cast<uintptr_t>(Gate.suspend_doorbell);
  if (WindowOverlaps(Data, EpochPointer, sizeof(uint64_t)) ||
      WindowOverlaps(Data, DoorbellPointer, sizeof(uint32_t)) ||
      ObjectOverlap(StopPointer, sizeof(*Stop), EpochPointer, sizeof(uint64_t)) ||
      ObjectOverlap(StopPointer, sizeof(*Stop), DoorbellPointer, sizeof(uint32_t)))
    return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  return SWITCHYARD_FEX_OK;
}

static switchyard_fex_result ValidateNativeGateGeometry(
    switchyard_fex_thread* Thread, const switchyard_fex_register_window* Window,
    switchyard_fex_stop* Stop, switchyard_fex_register_window& Output) {
  // Preserve original validation/error precedence on same-host reentry, but
  // never read or publish a certificate while an execution is outstanding.
  if (ActiveExecutionThread) return ValidateNativeGateGeometryUncached(Thread, Window, Stop, Output);
  auto& Cached = Thread->NativeGeometry;
  if (Thread->NativeGeometryState.load(std::memory_order_acquire) == 2 &&
      Cached.Window == reinterpret_cast<uintptr_t>(Window) &&
      Cached.Output == reinterpret_cast<uintptr_t>(&Output) &&
      Cached.Stop == reinterpret_cast<uintptr_t>(Stop) &&
      Cached.Core == reinterpret_cast<uintptr_t>(Thread->CoreThread) &&
      Cached.Frame == reinterpret_cast<uintptr_t>(Thread->CoreThread->CurrentFrame) &&
      Window->size >= sizeof(*Window) && Window->version == SWITCHYARD_FEX_REGISTER_WINDOW_VERSION &&
      !Window->flags && !Window->reserved && Window->data_size == SWITCHYARD_FEX_REGISTER_WINDOW_DATA_SIZE_V1 &&
      Cached.Data == Window->data && Cached.GS == Window->gs_base &&
      Cached.GS == Thread->NativeGate.gs_base &&
      Cached.Epoch == Thread->NativeGate.mapping_epoch &&
      Cached.Doorbell == Thread->NativeGate.suspend_doorbell &&
      Stop->size >= sizeof(*Stop) && Stop->version == SWITCHYARD_FEX_STOP_VERSION) {
    Output = *Window;
    Output.gs_base = 0;
    return SWITCHYARD_FEX_OK;
  }
  const auto Result = ValidateNativeGateGeometryUncached(Thread, Window, Stop, Output);
  if (Result != SWITCHYARD_FEX_OK) return Result;
  // The old borrowed API can describe its descriptor inside opaque storage.
  // Do not write the certificate in that case; preserve the cold contract.
  const auto Pointer = reinterpret_cast<uintptr_t>(Window);
  if (ObjectOverlap(Pointer, sizeof(*Window), reinterpret_cast<uintptr_t>(&Output), sizeof(Output)) ||
      ObjectOverlap(Pointer, sizeof(*Window), reinterpret_cast<uintptr_t>(Thread), sizeof(*Thread)) ||
      ObjectOverlap(Pointer, sizeof(*Window), reinterpret_cast<uintptr_t>(Thread->Process), sizeof(*Thread->Process)) ||
      ObjectOverlap(Pointer, sizeof(*Window), reinterpret_cast<uintptr_t>(Thread->CoreThread), sizeof(*Thread->CoreThread)) ||
      ObjectOverlap(Pointer, sizeof(*Window), reinterpret_cast<uintptr_t>(Thread->CoreThread->CurrentFrame),
                    sizeof(*Thread->CoreThread->CurrentFrame))) return Result;
  uint32_t Empty {};
  if (!Thread->NativeGeometryState.compare_exchange_strong(Empty, 1, std::memory_order_acq_rel)) return Result;
  Cached = {Pointer, reinterpret_cast<uintptr_t>(&Output), reinterpret_cast<uintptr_t>(Stop),
            static_cast<uintptr_t>(Window->data), reinterpret_cast<uintptr_t>(Thread->CoreThread),
            reinterpret_cast<uintptr_t>(Thread->CoreThread->CurrentFrame),
            static_cast<uintptr_t>(Thread->NativeGate.mapping_epoch),
            static_cast<uintptr_t>(Thread->NativeGate.suspend_doorbell), Window->gs_base};
#ifdef SWITCHYARD_FEX_NATIVE_GEOMETRY_TEST
  switchyard_fex_geometry_before_publish_test(Thread);
#endif
  Thread->NativeGeometryState.store(2, std::memory_order_release);
  return Result;
}

switchyard_fex_result switchyard_fex_experiment_execute_export_window(
    switchyard_fex_thread* Thread, uint64_t Generation,
    switchyard_fex_stop* Stop, const switchyard_fex_register_window* Window) {
  const auto Valid = ValidateCompletionWindow(Thread, Stop, Window);
  if (Valid != SWITCHYARD_FEX_OK) return Valid;
  const ExecutionActivation Activation {Thread, Generation};
  if (!OwnsExecution(Activation) || !Thread->StateValid) return SWITCHYARD_FEX_ERROR_BUSY;
  // The descriptor is borrowed only at entry; generated execution cannot
  // change the authenticated output span used during stopped-state export.
  const auto Output = *Window;
  try {
    Thread->Process->Context->ExecuteThread(Thread->CoreThread);
  } catch (...) {
    FillStop(Stop, SWITCHYARD_FEX_STOP_INTERNAL, Thread->CoreThread);
    FinishActiveExecution(Activation);
    return SWITCHYARD_FEX_ERROR_INTERNAL;
  }
  return CompleteExecution(Activation, Stop, &Output);
}

switchyard_fex_result switchyard_fex_experiment_bind_native_gate(
    switchyard_fex_thread* Thread, const switchyard_fex_native_gate* Gate) {
  if (!Thread || !Thread->Process || !Thread->CoreThread || !Gate ||
      (reinterpret_cast<uintptr_t>(Gate) & (alignof(switchyard_fex_native_gate) - 1)) ||
      Gate->size != sizeof(*Gate)) return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  if (!pthread_equal(Thread->NativeGateOwner, pthread_self())) return SWITCHYARD_FEX_ERROR_BUSY;
  if (Gate->version != SWITCHYARD_FEX_NATIVE_GATE_VERSION) return SWITCHYARD_FEX_ERROR_ABI_MISMATCH;
  if (!Thread->Process->ExternalAdmission || Thread->Process->CustomDispatchEnabled)
    return SWITCHYARD_FEX_ERROR_UNSUPPORTED;
  if (Gate->flags || Gate->reserved || !Gate->expected_epoch || !Gate->gs_base ||
      !Gate->mapping_epoch || (Gate->mapping_epoch & (alignof(uint64_t) - 1)) ||
      Gate->mapping_epoch > std::numeric_limits<uintptr_t>::max() - (sizeof(uint64_t) - 1) ||
      !Gate->suspend_doorbell || (Gate->suspend_doorbell & (alignof(uint32_t) - 1)) ||
      Gate->suspend_doorbell > std::numeric_limits<uintptr_t>::max() - (sizeof(uint32_t) - 1))
    return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  if (ActiveExecutionThread) return SWITCHYARD_FEX_ERROR_BUSY;
  StateAdmission Admission {Thread};
  if (!Admission) return SWITCHYARD_FEX_ERROR_BUSY;
  const auto* Epoch = reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(Gate->mapping_epoch));
  if (__atomic_load_n(Epoch, __ATOMIC_ACQUIRE) != Gate->expected_epoch)
    return SWITCHYARD_FEX_ERROR_UNSUPPORTED;
  Thread->NativeGate = *Gate;
  return SWITCHYARD_FEX_OK;
}

switchyard_fex_result switchyard_fex_experiment_execute_native_gate(
    switchyard_fex_thread* Thread, const switchyard_fex_register_window* Window,
    switchyard_fex_stop* Stop) {
  if (!Thread || !Thread->Process || !Thread->CoreThread) return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  // Authenticate the real host owner before touching its cell or signal TLS.
  if (!pthread_equal(Thread->NativeGateOwner, pthread_self())) return SWITCHYARD_FEX_ERROR_BUSY;
  const auto& Gate = Thread->NativeGate;
  if (!Gate.expected_epoch) return SWITCHYARD_FEX_ERROR_UNSUPPORTED;
  switchyard_fex_register_window Output;
  const auto Valid = ValidateNativeGateGeometry(Thread, Window, Stop, Output);
  if (Valid != SWITCHYARD_FEX_OK) return Valid;
  const auto EpochPointer = static_cast<uintptr_t>(Gate.mapping_epoch);
  if (ActiveExecutionThread) return SWITCHYARD_FEX_ERROR_BUSY;
  uint64_t Token {};
  const int Acquired = switchyard_fex_admission_acquire(&Thread->Admission, true, &Token);
  if (!Acquired) return SWITCHYARD_FEX_ERROR_BUSY;
  if (Acquired < 0) return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  const ExecutionActivation Activation {Thread, Token >> 3};
  // Every acquired fast reservation must release+wake, even before import:
  // unlike the legacy prepare API, the embedding mutex is not held here.
  ActiveExecutionThread = Thread;
#ifdef SWITCHYARD_FEX_NATIVE_GATE_TEST
  switchyard_fex_native_gate_acquired_test(Thread);
#endif
  const auto* Epoch = reinterpret_cast<const uint64_t*>(EpochPointer);
  if (__atomic_load_n(Epoch, __ATOMIC_ACQUIRE) != Gate.expected_epoch) {
    FinishActiveExecution(Activation);
    return SWITCHYARD_FEX_ERROR_UNSUPPORTED;
  }
  const auto Imported = ImportRegisterWindowUnlocked(Thread, Window);
  if (Imported != SWITCHYARD_FEX_OK) {
    FinishActiveExecution(Activation);
    return Imported;
  }
  const switchyard_fex_execution Execution {
    sizeof(switchyard_fex_execution), SWITCHYARD_FEX_EXECUTION_VERSION, 0, 0, 0, Gate.suspend_doorbell};
  ExecutionActivation Initialized;
  const auto Prepared = InitializeOwnedExecution(Activation, &Execution, Initialized);
  if (Prepared != SWITCHYARD_FEX_OK) return Prepared;
  try {
    Thread->Process->Context->ExecuteThread(Thread->CoreThread);
  } catch (...) {
    FillStop(Stop, SWITCHYARD_FEX_STOP_INTERNAL, Thread->CoreThread);
    FinishActiveExecution(Activation);
    return SWITCHYARD_FEX_ERROR_INTERNAL;
  }
  return CompleteExecution(Activation, Stop, &Output);
}

switchyard_fex_result switchyard_fex_thread_execute(
    switchyard_fex_thread* Thread, const switchyard_fex_execution* Execution, switchyard_fex_stop* Stop) {
  if (!Stop || Stop->size < sizeof(*Stop)) return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  if (Stop->version != SWITCHYARD_FEX_STOP_VERSION) return SWITCHYARD_FEX_ERROR_ABI_MISMATCH;
  uint64_t Generation {};
  const auto Result = switchyard_fex_thread_prepare_execution(Thread, Execution, &Generation);
  if (Result != SWITCHYARD_FEX_OK) {
    if (Result == SWITCHYARD_FEX_ERROR_INTERNAL) FillStop(Stop, SWITCHYARD_FEX_STOP_INTERNAL, Thread->CoreThread);
    return Result;
  }
  return switchyard_fex_thread_execute_prepared(Thread, Generation, Stop);
}

switchyard_fex_result switchyard_fex_thread_prepare_dispatch(
    switchyard_fex_thread* Thread, const switchyard_fex_execution* Execution,
    uint64_t AuthenticatedX18, switchyard_fex_dispatch* Output) {
  if (!Thread || !Thread->Process || !AuthenticatedX18 || !Output ||
      Output->size < sizeof(*Output) || Output->flags || Output->reserved) {
    return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  }
  if (Output->version != SWITCHYARD_FEX_DISPATCH_VERSION) return SWITCHYARD_FEX_ERROR_ABI_MISMATCH;
  if (!Thread->Process->CustomDispatchEnabled) return SWITCHYARD_FEX_ERROR_UNSUPPORTED;
  DetachedDispatch Dispatch;
  const auto Result = PrepareDetachedDispatch(Thread, Execution, Dispatch, AuthenticatedX18);
  if (Result != SWITCHYARD_FEX_OK) return Result;
  *Output = {sizeof(*Output), SWITCHYARD_FEX_DISPATCH_VERSION, 0, 0,
             reinterpret_cast<uintptr_t>(Dispatch.Entry),
             reinterpret_cast<uintptr_t>(Dispatch.Frame), Dispatch.Activation.Generation};
  return SWITCHYARD_FEX_OK;
}

switchyard_fex_result switchyard_fex_thread_complete_dispatch(
    switchyard_fex_thread* Thread, uint64_t Generation, switchyard_fex_stop* Stop) {
  return CompleteExecution({Thread, Generation}, Stop);
}

switchyard_fex_result switchyard_fex_thread_query_jit_stack(
    switchyard_fex_thread* Thread,
    const switchyard_fex_arm64_host_context* HostContext,
    uint32_t Access, uint64_t, uint64_t* GuestRSP) {
  if (!Thread || !Thread->CoreThread || !Thread->Process || !HostContext ||
      !GuestRSP || HostContext->size < sizeof(*HostContext) ||
      HostContext->flags || HostContext->reserved || !HostContext->pc ||
      (HostContext->pc & 3) || (Access != 0 && Access != 1 && Access != 8)) {
    return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  }
  if (HostContext->version != SWITCHYARD_FEX_ARM64_HOST_CONTEXT_VERSION) {
    return SWITCHYARD_FEX_ERROR_ABI_MISMATCH;
  }
  if (!CurrentExecution(Thread).Generation) {
    return SWITCHYARD_FEX_ERROR_BUSY;
  }
  auto* Process = Thread->Process;
  auto* CoreThread = Thread->CoreThread;
  const auto& State = CoreThread->CurrentFrame->State;
  const auto& Config = Process->Signals.GetConfig();
  uint64_t Stack;

  if (Process->Context->IsAddressInCodeBuffer(CoreThread, HostContext->pc)) {
    constexpr size_t RSPIndex = 4;
    if (Config.SRAGPRCount > std::size(State.gregs)) {
      return SWITCHYARD_FEX_ERROR_INTERNAL;
    }
    if (Config.SRAGPRCount > RSPIndex) {
      const auto HostIndex = Config.SRAGPRMapping[RSPIndex];
      if (HostIndex >= std::size(HostContext->gpr)) {
        return SWITCHYARD_FEX_ERROR_INTERNAL;
      }
      Stack = HostContext->gpr[HostIndex];
    } else {
      Stack = State.gregs[RSPIndex];
    }
  } else {
    return SWITCHYARD_FEX_ERROR_UNSUPPORTED;
  }
  *GuestRSP = Stack;
  return SWITCHYARD_FEX_OK;
}

switchyard_fex_result switchyard_fex_thread_reconstruct_jit_fault(
    switchyard_fex_thread* Thread,
    const switchyard_fex_arm64_host_context* HostContext,
    uint32_t Signal, uint32_t Access, uint64_t HostFaultAddress,
    switchyard_fex_x64_state* Output, switchyard_fex_fault* Fault) {
  if (!Thread || !Thread->CoreThread || !Thread->Process || !HostContext ||
      !Output || !Fault || HostContext->size < sizeof(*HostContext) ||
      Output->size < sizeof(*Output) || Fault->size < sizeof(*Fault)) {
    return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  }
  if (HostContext->version != SWITCHYARD_FEX_ARM64_HOST_CONTEXT_VERSION ||
      Output->version != SWITCHYARD_FEX_STATE_VERSION ||
      Fault->version != SWITCHYARD_FEX_FAULT_VERSION) {
    return SWITCHYARD_FEX_ERROR_ABI_MISMATCH;
  }
  if (HostContext->flags != 0 || HostContext->reserved != 0 ||
      !HostContext->pc || (HostContext->pc & 3) ||
      (Access != 0 && Access != 1 && Access != 8)) {
    return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  }
  const auto Activation = CurrentExecution(Thread);
  if (!Activation.Generation) {
    return SWITCHYARD_FEX_ERROR_BUSY;
  }

  auto* Process = Thread->Process;
  auto* CoreThread = Thread->CoreThread;
  switchyard_fex_result Result = SWITCHYARD_FEX_OK;
  try {
    const bool WasInJIT =
      Process->Context->IsAddressInCodeBuffer(CoreThread, HostContext->pc);
    const auto& Config = Process->Signals.GetConfig();
    auto& State = CoreThread->CurrentFrame->State;
    /* Native decoder/compiler frames can own C++ locks and allocations. They
     * must never be abandoned based merely on a fault address near guest RIP.
     * Guest fetch permissions use QueryGuestExecutableRange and a normal stop. */
    if (!WasInJIT) {
      return SWITCHYARD_FEX_ERROR_UNSUPPORTED;
    }
    if (WasInJIT &&
        (Config.SRAGPRCount > std::size(State.gregs) ||
         Config.SRAFPRCount > std::size(State.xmm.sse.data))) {
      Result = SWITCHYARD_FEX_ERROR_INTERNAL;
    } else if (WasInJIT) {
      State.rip = Process->Context->RestoreRIPFromHostPC(CoreThread, HostContext->pc);
      for (size_t Index = 0; Index < Config.SRAGPRCount; ++Index) {
        const auto HostIndex = Config.SRAGPRMapping[Index];
        if (HostIndex >= std::size(HostContext->gpr)) {
          Result = SWITCHYARD_FEX_ERROR_INTERNAL;
          break;
        }
        State.gregs[Index] = HostContext->gpr[HostIndex];
      }
      if (Result == SWITCHYARD_FEX_OK) {
        for (size_t Index = 0; Index < Config.SRAFPRCount; ++Index) {
          const auto HostIndex = Config.SRAFPRMapping[Index];
          if (HostIndex >= std::size(HostContext->vector)) {
            Result = SWITCHYARD_FEX_ERROR_INTERNAL;
            break;
          }
          std::memcpy(State.xmm.sse.data[Index], &HostContext->vector[HostIndex],
                      sizeof(__uint128_t));
        }
      }
      if (Result == SWITCHYARD_FEX_OK) {
        const auto EFlags = Process->Context->ReconstructCompactedEFLAGS(
            CoreThread, true, HostContext->gpr, HostContext->pstate);
        Process->Context->SetFlagsFromCompactedEFLAGS(CoreThread, EFlags);
        Thread->StateValid = true;
      }
    }
    if (Result == SWITCHYARD_FEX_OK) {
      Thread->StateValid = true;
      Result = ExportStateUnlocked(Thread, Output);
    }
  } catch (...) {
    Result = SWITCHYARD_FEX_ERROR_INTERNAL;
  }

  if (Result == SWITCHYARD_FEX_OK) {
    std::memset(Fault, 0, sizeof(*Fault));
    Fault->size = sizeof(*Fault);
    Fault->version = SWITCHYARD_FEX_FAULT_VERSION;
    Fault->signal = Signal;
    Fault->access = Access;
    Fault->host_pc = HostContext->pc;
    Fault->guest_rip = Output->rip;
    Fault->host_address = HostFaultAddress;
    Fault->guest_address = GuestAddressForHostFault(Process, HostFaultAddress);
    if (!RestoreJITExecutePermission()) {
      Result = SWITCHYARD_FEX_ERROR_UNSUPPORTED;
    }
  }
  if (!FinishActiveExecution(Activation)) {
    return SWITCHYARD_FEX_ERROR_INTERNAL;
  }
  return Result;
}

switchyard_fex_result switchyard_fex_thread_repair_unaligned_tso(
    switchyard_fex_thread* Thread,
    switchyard_fex_arm64_host_context* HostContext) {
  if (!Thread || !Thread->CoreThread || !Thread->Process || !HostContext ||
      HostContext->size < sizeof(*HostContext)) {
    return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  }
  if (HostContext->version != SWITCHYARD_FEX_ARM64_HOST_CONTEXT_VERSION) {
    return SWITCHYARD_FEX_ERROR_ABI_MISMATCH;
  }
  if (HostContext->flags != 0 || HostContext->reserved != 0 ||
      HostContext->pc < sizeof(uint32_t) || (HostContext->pc & 3) != 0) {
    return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  }
  if (!CurrentExecution(Thread).Generation) {
    return SWITCHYARD_FEX_ERROR_BUSY;
  }

  auto* const Context = Thread->Process->Context.get();
  auto* const CoreThread = Thread->CoreThread;
  const uint64_t HostPC = HostContext->pc;
  if (!Context->IsAddressInCodeBuffer(CoreThread, HostPC) ||
      !Context->IsHostAddressInCurrentBlock(CoreThread, HostPC - sizeof(uint32_t),
                                        3 * sizeof(uint32_t))) {
    return SWITCHYARD_FEX_ERROR_UNSUPPORTED;
  }

  uint64_t GPRs[31];
  std::memcpy(GPRs, HostContext->gpr, sizeof(GPRs));
  std::optional<int32_t> Offset;
  try {
    FEXCore::Allocator::ScopedJITWrite WriteScope;
    Offset = FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(
      CoreThread, FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrierTSOOnly,
      HostPC, GPRs, true);
  } catch (...) {
    return SWITCHYARD_FEX_ERROR_INTERNAL;
  }
  if (!Offset || (*Offset != -4 && *Offset != 0)) {
    return SWITCHYARD_FEX_ERROR_UNSUPPORTED;
  }

  uint64_t ResumePC;
  if (*Offset < 0) {
    const uint64_t Magnitude = static_cast<uint64_t>(-static_cast<int64_t>(*Offset));
    if (HostPC < Magnitude) {
      return SWITCHYARD_FEX_ERROR_INTERNAL;
    }
    ResumePC = HostPC - Magnitude;
  } else {
    const uint64_t Magnitude = static_cast<uint64_t>(*Offset);
    if (HostPC > std::numeric_limits<uint64_t>::max() - Magnitude) {
      return SWITCHYARD_FEX_ERROR_INTERNAL;
    }
    ResumePC = HostPC + Magnitude;
  }
  if (!Context->IsHostAddressInCurrentBlock(CoreThread, ResumePC,
                                        sizeof(uint32_t))) {
    return SWITCHYARD_FEX_ERROR_INTERNAL;
  }

  std::memcpy(HostContext->gpr, GPRs, sizeof(GPRs));
  HostContext->pc = ResumePC;
  return SWITCHYARD_FEX_OK;
}

switchyard_fex_result switchyard_fex_thread_repair_callret_fault(
    switchyard_fex_thread* Thread,
    switchyard_fex_arm64_host_context* HostContext,
    uint32_t Access, uint64_t HostFaultAddress) {
  if (!Thread || !Thread->CoreThread || !HostContext ||
      HostContext->size < sizeof(*HostContext) || HostContext->flags ||
      HostContext->reserved || !HostContext->pc || (HostContext->pc & 3) ||
      Access > 1) {
    return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  }
  if (HostContext->version != SWITCHYARD_FEX_ARM64_HOST_CONTEXT_VERSION) {
    return SWITCHYARD_FEX_ERROR_ABI_MISMATCH;
  }
  if (!CurrentExecution(Thread).Generation) {
    return SWITCHYARD_FEX_ERROR_BUSY;
  }
  if (Thread->CallRetStackAllocation == MAP_FAILED) {
    return SWITCHYARD_FEX_ERROR_INTERNAL;
  }

  constexpr uint64_t Size = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
  const auto CallRetRegister = Thread->Process->Signals.GetConfig().CallRetStackRegister;
  if (CallRetRegister >= std::size(HostContext->gpr)) {
    return SWITCHYARD_FEX_ERROR_INTERNAL;
  }
  const uint64_t Base = reinterpret_cast<uintptr_t>(Thread->CoreThread->CallRetStackBase);
  const uint64_t End = Base + Size; // Checked allocation established by InitializeCallRetStack.
  const uint64_t Stack = HostContext->gpr[CallRetRegister];
  const bool Push = Access == 1 && Stack == Base &&
                    HostFaultAddress >= Base - 16 && HostFaultAddress < Base;
  const bool Pop = Access == 0 && Stack == End &&
                   HostFaultAddress >= End && HostFaultAddress < End + 16;
  if ((!Push && !Pop) ||
      !Thread->Process->Context->IsHostAddressInCurrentBlock(
        Thread->CoreThread, HostContext->pc, sizeof(uint32_t))) {
    return SWITCHYARD_FEX_ERROR_UNSUPPORTED;
  }
  const uint32_t Instruction = __atomic_load_n(
    reinterpret_cast<const uint32_t*>(HostContext->pc), __ATOMIC_ACQUIRE);
  // Ignore only Rt/Rt2. Authenticate the exact 64-bit pair operation and the
  // register selected by this emitter, not an assumed host register layout.
  constexpr uint32_t Mask = 0xffff83e0U;
  const uint32_t Expected = (Push ? 0xa9bf0000U : 0xa8c10000U) |
                            (static_cast<uint32_t>(CallRetRegister) << 5);
  if ((Instruction & Mask) != Expected) {
    return SWITCHYARD_FEX_ERROR_UNSUPPORTED;
  }

  const uint64_t Reset = Base + Size / 4;
  // The faulting thread exclusively owns this cache; discard its first hint
  // before a retried pop, without touching guest memory or the native stack.
  auto* Hint = reinterpret_cast<uint64_t*>(Reset);
  Hint[0] = 0;
  Hint[1] = 0;
  HostContext->gpr[CallRetRegister] = Reset;
  return SWITCHYARD_FEX_OK;
}

switchyard_fex_result switchyard_fex_process_invalidate_code(
    switchyard_fex_process* Process, uint64_t GuestAddress, uint64_t Length) {
  if (!Process || !Length || GuestAddress > std::numeric_limits<uint64_t>::max() - Length) {
    return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  }
  std::lock_guard ProcessLock(Process->Mutex);
  ClosedAdmissions Admissions {Process};
  if (Process->Destroying || !Admissions.Idle()) {
    return SWITCHYARD_FEX_ERROR_BUSY;
  }
  try {
    std::unique_lock InvalidationLock(Process->Context->GetCodeInvalidationMutex());
    Process->Context->InvalidateCodeBuffersCodeRange(GuestAddress, Length);
    for (const auto* Thread : Process->Threads) {
      Process->Context->InvalidateThreadCachedCodeRange(
          Thread->CoreThread, GuestAddress, Length);
    }
  } catch (const std::bad_alloc&) {
    return SWITCHYARD_FEX_ERROR_NO_MEMORY;
  } catch (...) {
    return SWITCHYARD_FEX_ERROR_INTERNAL;
  }
  return SWITCHYARD_FEX_OK;
}

switchyard_fex_result switchyard_fex_thread_clear_code_cache(switchyard_fex_thread* Thread) {
  if (!Thread || !Thread->Process || !Thread->CoreThread) {
    return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
  }
  auto* Process = Thread->Process;
  std::lock_guard Lock(Process->Mutex);
  ClosedAdmissions Admissions {Process};
  if (Process->Destroying || !Admissions.Idle()) {
    return SWITCHYARD_FEX_ERROR_BUSY;
  }
  try {
    Process->Context->ClearCodeCache(Thread->CoreThread, true);
  } catch (const std::bad_alloc&) {
    return SWITCHYARD_FEX_ERROR_NO_MEMORY;
  } catch (...) {
    return SWITCHYARD_FEX_ERROR_INTERNAL;
  }
  return SWITCHYARD_FEX_OK;
}

} // extern "C"
