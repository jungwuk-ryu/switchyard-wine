// SPDX-License-Identifier: MIT
#include "admission_contract.h"

void RunAdmissionTests(switchyard_fex_process* Process) {
  Check(test_admission_contract() == 0);
  constexpr unsigned Rounds = 10000;
  switchyard_fex_admission Cell {};
  std::barrier Gate {2};
  uint64_t Token {};
  int Acquired {};
  std::thread Executor([&]() {
    for (unsigned i = 0; i < Rounds; ++i) {
      Gate.arrive_and_wait();
      Acquired = switchyard_fex_admission_acquire(&Cell, true, &Token);
      Gate.arrive_and_wait();
      Gate.arrive_and_wait();
      // Release races a reopen, then a fresh close. CLOSED must never be lost.
      if (Acquired) Check(switchyard_fex_admission_release(&Cell, Token));
      Gate.arrive_and_wait();
    }
  });
  for (unsigned i = 0; i < Rounds; ++i) {
    Gate.arrive_and_wait();
    switchyard_fex_admission_close(&Cell);
    Gate.arrive_and_wait();
    const auto Closed = switchyard_fex_admission_load(&Cell);
    Check(Closed & SWITCHYARD_FEX_ADMISSION_CLOSED);
    Check(bool(Closed & SWITCHYARD_FEX_ADMISSION_ACTIVE) == bool(Acquired));
    Gate.arrive_and_wait();
    switchyard_fex_admission_reopen(&Cell);
    switchyard_fex_admission_close(&Cell);
    Gate.arrive_and_wait();
    Check((switchyard_fex_admission_load(&Cell) & SWITCHYARD_FEX_ADMISSION_FLAGS) ==
          SWITCHYARD_FEX_ADMISSION_CLOSED);
    switchyard_fex_admission_reopen(&Cell);
  }
  Executor.join();

  alignas(16) uint8_t Code[] = {0x48, 0xb8, 0x42, 0, 0, 0, 0, 0, 0, 0, 0xf4};
  ExecutableFixture Current {Code, sizeof(Code)};
  Require(switchyard_fex_process_set_executable_range_query(Process, QueryCode, &Current));
  switchyard_fex_thread *First {}, *Second {};
  Require(switchyard_fex_thread_create(Process, &First));
  Require(switchyard_fex_thread_create(Process, &Second));
  switchyard_fex_x64_state Input {}, Output {};
  Input.size = Output.size = sizeof(Input);
  Input.version = Output.version = SWITCHYARD_FEX_STATE_VERSION;
  Input.rip = reinterpret_cast<uintptr_t>(Code);
  Input.rflags = 0x202;
  Input.mxcsr = 0x1f80;
  Input.fcw = 0x37f;
  Input.segment[1] = FEXCore::Core::CPUState::DEFAULT_USER_CS << 3;
  alignas(16) uint8_t GuestStack[4096] {};
  Input.gpr[4] = reinterpret_cast<uintptr_t>(GuestStack + sizeof(GuestStack));
  std::atomic_uint Doorbell {};
  switchyard_fex_execution Execution {};
  Execution.size = sizeof(Execution);
  Execution.version = SWITCHYARD_FEX_EXECUTION_VERSION;
  Execution.expected_hlt_rip = Input.rip + sizeof(Code) - 1;
  Execution.suspend_doorbell = reinterpret_cast<uintptr_t>(&Doorbell);
  switchyard_fex_stop Stop {};
  Stop.size = sizeof(Stop);
  Stop.version = SWITCHYARD_FEX_STOP_VERSION;
  ExecutionActivation Activation {};
  // Missing state must release admission on this pre-publication error path.
  Check(BeginExecution(First, &Execution, Activation) == SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
  Check(!(switchyard_fex_admission_load(&First->Admission) & SWITCHYARD_FEX_ADMISSION_FLAGS));
  Require(switchyard_fex_thread_import_state(First, &Input));
  Require(switchyard_fex_thread_import_state(Second, &Input));
  Require(BeginExecution(First, &Execution, Activation));
  Check(switchyard_fex_process_destroy(Process) == SWITCHYARD_FEX_ERROR_BUSY);
  Check(switchyard_fex_thread_destroy(First) == SWITCHYARD_FEX_ERROR_BUSY);
  Check(switchyard_fex_process_invalidate_code(Process, Input.rip, sizeof(Code)) == SWITCHYARD_FEX_ERROR_BUSY);
  Check(switchyard_fex_thread_clear_code_cache(Second) == SWITCHYARD_FEX_ERROR_BUSY);
  // Every failed close-all must reopen even the idle cells, without consuming
  // the active generation. Other short state operations remain usable.
  Check(OwnsExecution(Activation));
  Require(switchyard_fex_thread_export_state(Second, &Output));
  Check(!(switchyard_fex_admission_load(&First->Admission) & SWITCHYARD_FEX_ADMISSION_CLOSED));
  const auto Old = Activation;
  {
    std::lock_guard Lock(Process->Mutex);
    ClosedAdmissions Closed {Process};
    Check(!Closed.Idle());
    Check(switchyard_fex_thread_import_state(Second, &Input) == SWITCHYARD_FEX_ERROR_BUSY);
    Check(switchyard_fex_thread_execute(Second, &Execution, &Stop) == SWITCHYARD_FEX_ERROR_BUSY);
    Check(FinishActiveExecution(Activation));
    Check(Closed.Idle());
    Check(switchyard_fex_thread_execute(Second, &Execution, &Stop) == SWITCHYARD_FEX_ERROR_BUSY);
    Check((switchyard_fex_admission_load(&First->Admission) & SWITCHYARD_FEX_ADMISSION_FLAGS) ==
          SWITCHYARD_FEX_ADMISSION_CLOSED);
    // Ordinary errors and exception unwinding both reopen through RAII.
  }
  Require(BeginExecution(First, &Execution, Activation));
  Check(!FinishActiveExecution(Old) && OwnsExecution(Activation));
  Check(FinishActiveExecution(Activation));
  try {
    std::lock_guard Lock(Process->Mutex);
    ClosedAdmissions Closed {Process};
    Check(Closed.Idle());
    throw 1;
  } catch (int) {}
  Require(switchyard_fex_thread_export_state(First, &Output));
  {
    StateAdmission State {Second};
    Check(bool(State));
    Check(switchyard_fex_process_invalidate_code(Process, Input.rip, sizeof(Code)) == SWITCHYARD_FEX_ERROR_BUSY);
    Check(switchyard_fex_thread_clear_code_cache(First) == SWITCHYARD_FEX_ERROR_BUSY);
  }
  // Actual JIT execution no longer acquires the process mutex. Holding it here
  // would deadlock the old BeginExecution; CTest imposes a bounded timeout.
  {
    std::lock_guard Lock(Process->Mutex);
    Require(switchyard_fex_thread_execute(First, &Execution, &Stop));
    Check(Stop.reason == SWITCHYARD_FEX_STOP_HLT);
  }
  Require(switchyard_fex_thread_export_state(First, &Output));
  Check(Output.gpr[0] == 0x42);

  // Contend real public execution against repeated invalidation. Each side can
  // return BUSY, but every admitted JIT run and every later reuse must be exact.
  constexpr unsigned JITRounds = 128;
  std::barrier Start {2};
  std::thread Mutator([&]() {
    for (unsigned i = 0; i < JITRounds; ++i) {
      Start.arrive_and_wait();
      const auto Result = switchyard_fex_process_invalidate_code(Process, Input.rip, sizeof(Code));
      Check(Result == SWITCHYARD_FEX_OK || Result == SWITCHYARD_FEX_ERROR_BUSY);
      Start.arrive_and_wait();
    }
  });
  unsigned Completed {};
  for (unsigned i = 0; i < JITRounds; ++i) {
    Require(switchyard_fex_thread_import_state(First, &Input));
    Start.arrive_and_wait();
    const auto Result = switchyard_fex_thread_execute(First, &Execution, &Stop);
    Check(Result == SWITCHYARD_FEX_OK || Result == SWITCHYARD_FEX_ERROR_BUSY);
    Start.arrive_and_wait();
    if (Result == SWITCHYARD_FEX_OK) ++Completed;
    // The mutator is quiescent until the next round. A rejected attempt must
    // succeed on the first retry, and every round must produce the same state.
    else Require(switchyard_fex_thread_execute(First, &Execution, &Stop));
    Require(switchyard_fex_thread_export_state(First, &Output));
    Check(Stop.reason == SWITCHYARD_FEX_STOP_HLT && Output.gpr[0] == 0x42);
  }
  Mutator.join();
  // A deterministic post-contention replay checks closure rollback and cache
  // reuse even on a scheduler that ran either participant to completion first.
  Code[2] = 0x43;
  Require(switchyard_fex_process_invalidate_code(Process, Input.rip, sizeof(Code)));
  Require(switchyard_fex_thread_clear_code_cache(First));
  Require(switchyard_fex_thread_import_state(First, &Input));
  Require(switchyard_fex_thread_execute(First, &Execution, &Stop));
  Require(switchyard_fex_thread_export_state(First, &Output));
  Check(Stop.reason == SWITCHYARD_FEX_STOP_HLT && Output.gpr[0] == 0x43);
  // Wine removes a binding before destroying its adapter thread. A concurrent
  // invalidator must delay that destruction, not return a transient BUSY which
  // would strand an unowned member in the adapter's process list.
  std::latch DestroyStarted {1};
  std::future<switchyard_fex_result> Destroyed;
  {
    std::lock_guard Lock(Process->Mutex);
    ClosedAdmissions Closed {Process};
    Check(Closed.Idle());
    Destroyed = std::async(std::launch::async, [&]() {
      DestroyStarted.count_down();
      return switchyard_fex_thread_destroy(Second);
    });
    DestroyStarted.wait();
    Check(Destroyed.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
  }
  Require(Destroyed.get());
  Require(switchyard_fex_thread_destroy(First));
  std::printf("Admission: %u close/entry/exit races; %u/%u JIT admissions won, every round/retry exact; generation/rollback/detach passed\n",
              Rounds, Completed, JITRounds);
}
