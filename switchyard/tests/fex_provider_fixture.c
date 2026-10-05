#include "switchyard_fex.h"

extern int ntdll_fixture(void);

__attribute__((used, visibility("default"))) const char fixture_process_abi[] =
    SWITCHYARD_FIXTURE_ABI_IDENTITY;

#ifndef SWITCHYARD_FIXTURE_OMIT_API
/* Preserve each real C ABI type; never call through a forged fixture cast. */
#define IMPORT(name) __attribute__((used, visibility("default"))) \
    __typeof__(&name) const fixture_##name = name
IMPORT(switchyard_fex_abi_version);
IMPORT(switchyard_fex_provider_abi_identity);
IMPORT(switchyard_fex_upstream_revision);
IMPORT(switchyard_fex_result_string);
IMPORT(switchyard_fex_process_create);
IMPORT(switchyard_fex_process_destroy);
IMPORT(switchyard_fex_process_invalidate_code);
IMPORT(switchyard_fex_process_set_executable_range_query);
IMPORT(switchyard_fex_thread_create_with_domain);
IMPORT(switchyard_fex_thread_destroy);
IMPORT(switchyard_fex_thread_prepare_execution);
IMPORT(switchyard_fex_thread_execute_prepared);
IMPORT(switchyard_fex_thread_import_state);
IMPORT(switchyard_fex_thread_import_register_window);
IMPORT(switchyard_fex_thread_export_register_window);
IMPORT(switchyard_fex_thread_export_state);
IMPORT(switchyard_fex_thread_prepare_dispatch);
IMPORT(switchyard_fex_thread_complete_dispatch);
IMPORT(switchyard_fex_thread_query_jit_stack);
IMPORT(switchyard_fex_thread_reconstruct_jit_fault);
IMPORT(switchyard_fex_thread_repair_callret_fault);
IMPORT(switchyard_fex_thread_repair_unaligned_tso);
#endif

__attribute__((visibility("default"))) unsigned int provider_fixture(void)
{
    return (unsigned int)ntdll_fixture() + switchyard_fex_abi_version();
}
