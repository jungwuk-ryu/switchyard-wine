// SPDX-License-Identifier: MIT
struct SharedWakeFixture {
  switchyard_fex_admission* Cell {};
  unsigned Count {};
};

void SharedWake(void* Context) {
  auto& Fixture = *static_cast<SharedWakeFixture*>(Context);
  Check((switchyard_fex_admission_load(Fixture.Cell) & SWITCHYARD_FEX_ADMISSION_FLAGS) ==
        SWITCHYARD_FEX_ADMISSION_CLOSED);
  ++Fixture.Count;
}

void RunSharedAdmissionTests(switchyard_fex_process* Process) {
  alignas(16) uint8_t Code[] = {0x48, 0xb8, 0x42, 0, 0, 0, 0, 0, 0, 0, 0xf4};
  ExecutableFixture Current {Code, sizeof(Code)};
  Require(switchyard_fex_process_set_executable_range_query(Process, QueryCode, &Current));
  SharedWakeFixture Wake;
  switchyard_fex_execution_domain Domain {};
  Domain.size = sizeof(Domain);
  Domain.version = SWITCHYARD_FEX_DOMAIN_VERSION;
  Domain.wake = SharedWake;
  Domain.context = &Wake;
  switchyard_fex_thread *First {}, *Second {};
  switchyard_fex_admission* SecondCell {};
  Check(switchyard_fex_thread_create(Process, &First) == SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT && !First);
  for (unsigned Defect = 0; Defect < 5; ++Defect) {
    auto Bad = Domain;
    if (Defect == 0) --Bad.size;
    if (Defect == 1) ++Bad.version;
    if (Defect == 2) Bad.flags = 1;
    if (Defect == 3) Bad.reserved = 1;
    if (Defect == 4) Bad.wake = nullptr;
    const auto Expected = Defect == 1 ? SWITCHYARD_FEX_ERROR_ABI_MISMATCH : SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
    Check(switchyard_fex_thread_create_with_domain(Process, &Bad, &First, &Wake.Cell) == Expected);
    Check(!First && !Wake.Cell && Process->Threads.empty());
  }
  Require(switchyard_fex_thread_create_with_domain(Process, &Domain, &First, &Wake.Cell));
  Require(switchyard_fex_thread_create_with_domain(Process, &Domain, &Second, &SecondCell));
  Check(Wake.Cell == &First->Admission && SecondCell == &Second->Admission);
  Check(switchyard_fex_thread_destroy(First) == SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
  // Even an idle external member must be closed; the adapter never supplies or
  // rolls back that closure on behalf of an embedding.
  Check(switchyard_fex_process_invalidate_code(Process, reinterpret_cast<uintptr_t>(Code), sizeof(Code)) ==
        SWITCHYARD_FEX_ERROR_BUSY);
  Check(switchyard_fex_admission_load(Wake.Cell) == 0 && switchyard_fex_admission_load(SecondCell) == 0);
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
  switchyard_fex_execution Execution {};
  Execution.size = sizeof(Execution);
  Execution.version = SWITCHYARD_FEX_EXECUTION_VERSION;
  Execution.expected_hlt_rip = Input.rip + sizeof(Code) - 1;
  std::atomic_uint Doorbell {};
  Execution.suspend_doorbell = reinterpret_cast<uintptr_t>(&Doorbell);
  switchyard_fex_stop Stop {};
  Stop.size = sizeof(Stop);
  Stop.version = SWITCHYARD_FEX_STOP_VERSION;
  uint64_t Generation = UINT64_MAX;
  Check(switchyard_fex_thread_prepare_execution(First, &Execution, &Generation) ==
        SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT && Generation == UINT64_MAX);
  Require(switchyard_fex_thread_import_state(First, &Input));
  Require(switchyard_fex_thread_prepare_execution(First, &Execution, &Generation));
  const auto OldGeneration = Generation;
  Check(Generation && Generation != UINT64_MAX);
  Check(switchyard_fex_thread_execute_prepared(First, Generation - 1, &Stop) == SWITCHYARD_FEX_ERROR_BUSY);
  auto Other = std::async(std::launch::async, [&]() {
    switchyard_fex_stop OtherStop = Stop;
    return switchyard_fex_thread_execute_prepared(First, Generation, &OtherStop);
  });
  Check(Other.get() == SWITCHYARD_FEX_ERROR_BUSY);
  --Stop.size;
  Check(switchyard_fex_thread_execute_prepared(First, Generation, &Stop) == SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT);
  ++Stop.size;
  switchyard_fex_admission_close(Wake.Cell);
  switchyard_fex_admission_close(SecondCell);
  Check(switchyard_fex_thread_destroy(First) == SWITCHYARD_FEX_ERROR_BUSY);
  Check(switchyard_fex_process_invalidate_code(Process, Input.rip, sizeof(Code)) == SWITCHYARD_FEX_ERROR_BUSY);
  Check(switchyard_fex_thread_import_state(Second, &Input) == SWITCHYARD_FEX_ERROR_BUSY);
  Require(switchyard_fex_thread_execute_prepared(First, Generation, &Stop));
  Check(Stop.reason == SWITCHYARD_FEX_STOP_HLT && Wake.Count == 1);
  Check(switchyard_fex_thread_execute_prepared(First, Generation, &Stop) == SWITCHYARD_FEX_ERROR_BUSY);
  Require(switchyard_fex_process_invalidate_code(Process, Input.rip, sizeof(Code)));
  Require(switchyard_fex_thread_clear_code_cache(First));
  Check((switchyard_fex_admission_load(Wake.Cell) & SWITCHYARD_FEX_ADMISSION_FLAGS) == SWITCHYARD_FEX_ADMISSION_CLOSED);
  Check(switchyard_fex_admission_load(SecondCell) == SWITCHYARD_FEX_ADMISSION_CLOSED);
  switchyard_fex_admission_reopen(Wake.Cell);
  switchyard_fex_admission_reopen(SecondCell);
  Require(switchyard_fex_thread_export_state(First, &Output));
  Check(Output.gpr[0] == 0x42 && Wake.Count == 1);
  for (unsigned i = 0; i < 128; ++i) {
    Require(switchyard_fex_thread_import_state(First, &Input));
    Require(switchyard_fex_thread_prepare_execution(First, &Execution, &Generation));
    Check(switchyard_fex_thread_execute_prepared(First, OldGeneration, &Stop) == SWITCHYARD_FEX_ERROR_BUSY);
    Require(switchyard_fex_thread_execute_prepared(First, Generation, &Stop));
    Require(switchyard_fex_thread_export_state(First, &Output));
    Check(Stop.reason == SWITCHYARD_FEX_STOP_HLT && Output.gpr[0] == 0x42 && Wake.Count == 1);
  }
  switchyard_fex_admission_close(Wake.Cell);
  switchyard_fex_admission_close(SecondCell);
  Require(switchyard_fex_thread_destroy(Second));
  Require(switchyard_fex_thread_destroy(First));
  Check(Wake.Count == 1);
  std::puts("Shared admission: external closure, exact wake, reserved execution, stale/wrong-owner rejection, teardown passed");
}
