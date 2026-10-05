// SPDX-License-Identifier: MIT
// Actual adapter validators, cold/warm negative equivalence and real bind reset.
#include "switchyard_fex.h"
static void switchyard_fex_geometry_before_publish_test(switchyard_fex_thread*);
#define SWITCHYARD_FEX_NATIVE_GEOMETRY_TEST 1
#include "../src/switchyard_fex.cpp"
#include <array>
#include <csetjmp>
#include <source_location>

static void Check(bool Value, const std::source_location Site = std::source_location::current()) {
  if (Value) return;
  std::fprintf(stderr, "native geometry line %u\n", Site.line());
  std::_Exit(97);
}
static void Wake(void*) {}
static std::jmp_buf Abandon;
static bool AbandonPublication;
static void switchyard_fex_geometry_before_publish_test(switchyard_fex_thread* Thread) {
  Check(Thread->NativeGeometryState.load(std::memory_order_acquire) == 1);
  if (AbandonPublication) std::longjmp(Abandon, 1);
}
int main() {
  switchyard_fex_config Config {.size = sizeof(Config), .abi_version = SWITCHYARD_FEX_ABI_VERSION,
                               .flags = SWITCHYARD_FEX_CONFIG_EXTERNAL_ADMISSION};
  switchyard_fex_process* Process {};
  switchyard_fex_thread* Thread {};
  switchyard_fex_execution_domain Domain {sizeof(Domain), SWITCHYARD_FEX_DOMAIN_VERSION, 0, 0, Wake, nullptr};
  switchyard_fex_admission* Cell {};
  Check(switchyard_fex_process_create(&Config, &Process) == SWITCHYARD_FEX_OK);
  Check(switchyard_fex_thread_create_with_domain(Process, &Domain, &Thread, &Cell) == SWITCHYARD_FEX_OK);
  uint64_t Epoch = 1;
  uint32_t Doorbell {};
  switchyard_fex_native_gate Gate {sizeof(Gate), SWITCHYARD_FEX_NATIVE_GATE_VERSION, 0, 0,
                                 reinterpret_cast<uintptr_t>(&Epoch), Epoch, 0x12345000,
                                 reinterpret_cast<uintptr_t>(&Doorbell)};
  Check(switchyard_fex_experiment_bind_native_gate(Thread, &Gate) == SWITCHYARD_FEX_OK);
  std::array<unsigned char, 816> Bytes {};
  switchyard_fex_register_window Window {sizeof(Window), SWITCHYARD_FEX_REGISTER_WINDOW_VERSION, 0, 0,
      reinterpret_cast<uintptr_t>(Bytes.data()), 408, Gate.gs_base};
  const auto Original = Window;
  switchyard_fex_stop Stop {.size = sizeof(Stop), .version = SWITCHYARD_FEX_STOP_VERSION};
  switchyard_fex_register_window Output {}, Reference {};
  unsigned Cases {}, Hits {};
  for (unsigned Pass = 0; Pass < 64; ++Pass) {
    for (unsigned Kind = 0; Kind < 40; ++Kind) {
      Window = Original;
      Stop = {.size = sizeof(Stop), .version = SWITCHYARD_FEX_STOP_VERSION};
      Check(ValidateNativeGateGeometry(Thread, &Window, &Stop, Output) == SWITCHYARD_FEX_OK);
      Check(Thread->NativeGeometryState.load(std::memory_order_acquire) == 2);
      const auto Geometry = Thread->NativeGeometry;
      // Same descriptor addresses with independently changed contents.
      const switchyard_fex_register_window* Input = &Window;
      switchyard_fex_stop* Target = &Stop;
      switch (Kind) {
      case 0: ++Hits; break;
      case 1: Window.size = sizeof(Window) - 1; break;
      case 2: Window.version = 0; break;
      case 3: Window.flags = 1; break;
      case 4: Window.reserved = 1; break;
      case 5: Window.data_size = 407; break;
      case 6: Window.data_size = 409; break;
      case 7: Window.data = 0; break;
      case 8: Window.data = UINT64_MAX - 406; break;
      case 9: Window.gs_base ^= 1; break;
      case 10: Window.data = reinterpret_cast<uintptr_t>(&Window); break;
      case 11: Window.data = reinterpret_cast<uintptr_t>(&Stop); break;
      case 12: Window.data = reinterpret_cast<uintptr_t>(Thread); break;
      case 13: Window.data = reinterpret_cast<uintptr_t>(Process); break;
      case 14: Window.data = reinterpret_cast<uintptr_t>(Thread->CoreThread); break;
      case 15: Window.data = reinterpret_cast<uintptr_t>(Thread->CoreThread->CurrentFrame); break;
      case 16: Window.data = reinterpret_cast<uintptr_t>(&Epoch); break;
      case 17: Window.data = reinterpret_cast<uintptr_t>(&Doorbell); break;
      case 18: Window.data = reinterpret_cast<uintptr_t>(&Output); break;
      case 19: Stop.size = sizeof(Stop) - 1; break;
      case 20: Stop.version = 0; break;
      case 21: Input = nullptr; break;
      case 22: Target = nullptr; break;
      case 23: Input = reinterpret_cast<const switchyard_fex_register_window*>(reinterpret_cast<uintptr_t>(&Window) + 1); break;
      case 24: Target = reinterpret_cast<switchyard_fex_stop*>(reinterpret_cast<uintptr_t>(&Stop) + 1); break;
      case 25: Target = reinterpret_cast<switchyard_fex_stop*>(&Window); break;
      // Expanded compatible structures, misaligned payload and changed spans
      // are not errors; the cold validator decides each unchanged contract.
      case 26: Window.size += 8; break;
      case 27: Stop.size += 8; break;
      case 28: Window.data += 1; break;
      case 29: Window.data += 408; break;
      case 30: Window.data = reinterpret_cast<uintptr_t>(Thread) - 407; break;
      case 31: Window.data = reinterpret_cast<uintptr_t>(Thread) + sizeof(*Thread) - 1; break;
      case 32: Window.data = reinterpret_cast<uintptr_t>(&Epoch) - 407; break;
      case 33: Window.data = reinterpret_cast<uintptr_t>(&Doorbell) + sizeof(Doorbell) - 1; break;
      case 34: Window.data = reinterpret_cast<uintptr_t>(&Stop) + sizeof(Stop) - 1; break;
      case 35: Window.data = reinterpret_cast<uintptr_t>(&Window) + sizeof(Window) - 1; break;
      case 36: Window.data = UINT64_MAX - 407; break;
      case 37: Window.data = reinterpret_cast<uintptr_t>(&Output) + sizeof(Output) - 1; break;
      case 38: Window.flags = UINT32_MAX; break;
      case 39: Stop.version = UINT32_MAX; break;
      }
      // The original Output object address is itself part of the geometry.
      // Both validators use it in turn, so a payload alias has the same oracle.
      Reference = Output;
      const auto Expected = ValidateNativeGateGeometryUncached(Thread, Input, Target, Output);
      const auto ExpectedOutput = Output;
      Output = Reference;
      const auto Actual = ValidateNativeGateGeometry(Thread, Input, Target, Output);
      Check(Actual == Expected);
      if (Actual == SWITCHYARD_FEX_OK)
        Check(std::memcmp(&Output, &ExpectedOutput, sizeof(Output)) == 0);
      else
        Check(std::memcmp(&Geometry, &Thread->NativeGeometry, sizeof(Geometry)) == 0);
      ++Cases;
    }
  }
  Window = Original;
  Stop = {.size = sizeof(Stop), .version = SWITCHYARD_FEX_STOP_VERSION};
  Check(ValidateNativeGateGeometry(Thread, &Window, &Stop, Output) == SWITCHYARD_FEX_OK);
  const auto Before = Thread->NativeGeometry;
  ActiveExecutionThread = Thread;
  auto Other = Window;
  Check(ValidateNativeGateGeometry(Thread, &Other, &Stop, Output) == SWITCHYARD_FEX_OK);
  Check(std::memcmp(&Before, &Thread->NativeGeometry, sizeof(Before)) == 0);
  ActiveExecutionThread = nullptr;
  ++Gate.gs_base;
  Check(switchyard_fex_experiment_bind_native_gate(Thread, &Gate) == SWITCHYARD_FEX_OK);
  Check(Thread->NativeGeometryState.load(std::memory_order_acquire) == 2);
  Check(ValidateNativeGateGeometry(Thread, &Window, &Stop, Output) == SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
  Window.gs_base = Gate.gs_base;
  Check(ValidateNativeGateGeometry(Thread, &Window, &Stop, Output) == SWITCHYARD_FEX_OK);
  Check(Thread->NativeGeometryState.load(std::memory_order_acquire) == 2 && Thread->NativeGeometry.GS == Original.gs_base);
  switchyard_fex_admission_close(Cell);
  Check(switchyard_fex_thread_destroy(Thread) == SWITCHYARD_FEX_OK);
  Check(switchyard_fex_thread_create_with_domain(Process, &Domain, &Thread, &Cell) == SWITCHYARD_FEX_OK);
  Check(switchyard_fex_experiment_bind_native_gate(Thread, &Gate) == SWITCHYARD_FEX_OK);
  AbandonPublication = true;
  if (!setjmp(Abandon)) {
    (void)ValidateNativeGateGeometry(Thread, &Window, &Stop, Output);
    Check(false);
  }
  AbandonPublication = false;
  Check(Thread->NativeGeometryState.load(std::memory_order_acquire) == 1);
  Check(!ActiveExecutionThread && !(switchyard_fex_admission_load(Cell) & SWITCHYARD_FEX_ADMISSION_FLAGS));
  for (unsigned Index = 0; Index < 64; ++Index) {
    Check(ValidateNativeGateGeometry(Thread, &Window, &Stop, Output) == SWITCHYARD_FEX_OK);
    Check(Thread->NativeGeometryState.load(std::memory_order_acquire) == 1);
  }
  switchyard_fex_admission_close(Cell);
  Check(switchyard_fex_thread_destroy(Thread) == SWITCHYARD_FEX_OK);
  Check(switchyard_fex_process_destroy(Process) == SWITCHYARD_FEX_OK);
  Check(Cases == 2560 && Hits == 64);
  std::puts("native_geometry result=pass cases=2560 same_address_mutations=1 reentry_unchanged=1 bind_changed_cold=1 abandonment_cold=64 immutable=1");
}
