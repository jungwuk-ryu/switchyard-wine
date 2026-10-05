// SPDX-License-Identifier: MIT
// Actual adapter: deterministic post-admission closure and epoch TOCTOU oracle.
#include "switchyard_fex.h"
static void switchyard_fex_native_gate_acquired_test(switchyard_fex_thread*);
#define SWITCHYARD_FEX_NATIVE_GATE_TEST 1
#include "../src/switchyard_fex.cpp"
#include <array>
#include <source_location>

static void Check(bool Value, const std::source_location Site = std::source_location::current()) {
  if (Value) return;
  std::fprintf(stderr, "native gate state line %u\n", Site.line());
  std::_Exit(97);
}
static uint64_t Epoch = 1;
static uint32_t Doorbell;
static std::mutex Mutex;
static switchyard_fex_admission* Cell;
static bool CloseAfterAcquire;
static unsigned Wakes;
static void switchyard_fex_native_gate_acquired_test(switchyard_fex_thread* Thread) {
  Check(ActiveExecutionThread == Thread && Cell == &Thread->Admission);
  Check((switchyard_fex_admission_load(Cell) & SWITCHYARD_FEX_ADMISSION_FLAGS) == 3);
  if (!CloseAfterAcquire) return;
  std::lock_guard Lock(Mutex);
  __atomic_add_fetch(&Epoch, uint64_t {1}, __ATOMIC_RELEASE);
  switchyard_fex_admission_close(Cell);
}
static void Wake(void*) {
  Check(!ActiveExecutionThread &&
        (switchyard_fex_admission_load(Cell) & SWITCHYARD_FEX_ADMISSION_FLAGS) == 4);
  std::lock_guard Lock(Mutex); // SDK must not retain the embedding mutex.
  ++Wakes;
}
int main() {
  switchyard_fex_config Config {.size = sizeof(Config), .abi_version = SWITCHYARD_FEX_ABI_VERSION,
                               .flags = SWITCHYARD_FEX_CONFIG_EXTERNAL_ADMISSION};
  switchyard_fex_process* Process {};
  switchyard_fex_thread* Thread {};
  switchyard_fex_execution_domain Domain {sizeof(Domain), SWITCHYARD_FEX_DOMAIN_VERSION, 0, 0, Wake, nullptr};
  Check(switchyard_fex_process_create(&Config, &Process) == SWITCHYARD_FEX_OK);
  Check(switchyard_fex_thread_create_with_domain(Process, &Domain, &Thread, &Cell) == SWITCHYARD_FEX_OK);
  switchyard_fex_native_gate Gate {sizeof(Gate), SWITCHYARD_FEX_NATIVE_GATE_VERSION, 0, 0,
                                 reinterpret_cast<uintptr_t>(&Epoch), Epoch, 0x12345000,
                                 reinterpret_cast<uintptr_t>(&Doorbell)};
  std::array<unsigned char, 408> Bytes;
  switchyard_fex_register_window Window {sizeof(Window), SWITCHYARD_FEX_REGISTER_WINDOW_VERSION, 0, 0,
                                        reinterpret_cast<uintptr_t>(Bytes.data()), Bytes.size(), Gate.gs_base};
  auto& Frame = *Thread->CoreThread->CurrentFrame;
  std::array<unsigned char, sizeof(Frame)> Original;
  std::memcpy(Original.data(), &Frame, sizeof(Frame));
  unsigned Cases {};
  for (unsigned Index = 0; Index < 64; ++Index) {
    {
      std::lock_guard Lock(Mutex);
      Gate.expected_epoch = __atomic_load_n(&Epoch, __ATOMIC_ACQUIRE);
      Check(switchyard_fex_experiment_bind_native_gate(Thread, &Gate) == SWITCHYARD_FEX_OK);
    }
    // Randomized whole bytes, including every SRA and fault-info field. A stale
    // epoch must reject before clearing anything, not only before guest entry.
    auto* Raw = reinterpret_cast<unsigned char*>(&Frame);
    for (size_t Byte = 0; Byte < sizeof(Frame); ++Byte)
      Raw[Byte] = static_cast<unsigned char>(Index * 29u + Byte * 17u);
    Frame.ExternalSuspendDoorbell = 0;
    Frame.ExternalStop = FEXCore::Core::ExternalStopReason::None;
    Thread->StateValid = (Index & 1u) != 0;
    std::array<unsigned char, sizeof(Frame)> Before;
    std::memcpy(Before.data(), &Frame, sizeof(Frame));
    Bytes.fill(0xa5);
    const auto BeforeBytes = Bytes;
    switchyard_fex_stop Stop {.size = sizeof(Stop), .version = SWITCHYARD_FEX_STOP_VERSION,
                             .reason = 0xcafe, .rip = 0xabcddcba};
    const auto BeforeStop = Stop;
    const uint64_t Word = switchyard_fex_admission_load(Cell);
    CloseAfterAcquire = true;
    Check(switchyard_fex_experiment_execute_native_gate(Thread, &Window, &Stop) == SWITCHYARD_FEX_ERROR_UNSUPPORTED);
    CloseAfterAcquire = false;
    Check(!ActiveExecutionThread && Wakes == Index + 1 &&
          switchyard_fex_admission_load(Cell) == (Word + 8u + 4u));
    Check(Bytes == BeforeBytes && std::memcmp(&Stop, &BeforeStop, sizeof(Stop)) == 0 &&
          std::memcmp(&Frame, Before.data(), sizeof(Frame)) == 0 &&
          Thread->StateValid == ((Index & 1u) != 0));
    {
      std::lock_guard Lock(Mutex);
      switchyard_fex_admission_reopen(Cell);
    }
    ++Cases;
  }
  // Restore valid frame state before the actual core teardown uses it.
  std::memcpy(&Frame, Original.data(), sizeof(Frame));
  switchyard_fex_admission_close(Cell);
  Check(switchyard_fex_thread_destroy(Thread) == SWITCHYARD_FEX_OK);
  Check(switchyard_fex_process_destroy(Process) == SWITCHYARD_FEX_OK);
  Check(Cases == 64 && Wakes == 64);
  std::puts("native_gate_state result=pass post_acquire_closures=64 wakes=64 output_unchanged=64");
}
