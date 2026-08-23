#!/usr/bin/env python3
"""Verify the bounded ARM64EC fixed-low signal bridge integration."""

from pathlib import Path
import sys


def require(source: str, token: str) -> int:
    offset = source.find(token)
    if offset < 0:
        raise AssertionError(f"missing required bridge contract: {token}")
    return offset


def section(source: str, begin_token: str, end_token: str) -> str:
    begin = require(source, begin_token)
    end = require(source[begin:], end_token) + begin
    return source[begin:end]


def verify(signal_source: str, virtual_source: str) -> None:
    # The compiled decoder test owns opcode, immediate, FAR, overflow, span,
    # and writeback behavior.  Keep this check focused on integration.
    require(signal_source, '# include "arm64ec_low_guest_decode.h"')
    handler = section(
        signal_source, "static BOOL handle_arm64ec_low_guest_access", "static void segv_handler"
    )
    for token in (
        "arm64ec_low_guest_is_translation_fault( esr )",
        "virtual_arm64ec_fetch_low_guest_instr",
        "arm64ec_low_guest_base_register( instr )",
        "arm64ec_decode_low_guest_access( instr, base",
        "get_arm64_signal_reg( sigcontext, rn, &base )",
        "get_arm64_signal_q",
        "set_arm64_signal_q",
        "get_arm64_signal_d",
        "set_arm64_signal_d",
        "virtual_arm64ec_low_guest_access",
        "access.sign_extend_size",
        "arm64ec_low_guest_extend_signed_load",
        "PC_sig( sigcontext ) = pc + 4",
    ):
        require(handler, token)
    if "is_ec_code" in handler or "*(const ULONG *)pc" in handler:
        raise AssertionError("signal bridge trusts an unbounded bitmap or direct PC dereference")
    read_access = require(handler, "FALSE, &extra_status );")
    discard_check = require(handler, "else if (!status && access.rt != 31)")
    sign_extend = require(handler, "arm64ec_low_guest_extend_signed_load( value.word[0]")
    scalar_result = sign_extend + require(
        handler[sign_extend:], "set_arm64_signal_reg( sigcontext, access.rt, value.word[0] )"
    )
    writeback = require(handler, "if (access.writeback_valid")
    pc_advance = require(handler, "PC_sig( sigcontext ) = pc + 4")
    if not read_access < discard_check < sign_extend < scalar_result < writeback < pc_advance:
        raise AssertionError("signed scalar load result/writeback ordering is unsafe")
    for obsolete in (
        "static BOOL decode_arm64ec_low_guest_access",
        "static BOOL arm64ec_low_guest_translation_fault",
        "static BOOL arm64ec_low_guest_add",
    ):
        if obsolete in signal_source:
            raise AssertionError(f"signal handler duplicates tested decoder logic: {obsolete}")

    dabt = section(signal_source, "case ESR_ELx_EC_DABT_LOW:", "default:")
    require(dabt, "handle_arm64ec_low_guest_access( data, sigcontext, siginfo, esr")

    fetch = section(
        virtual_source,
        "BOOL virtual_arm64ec_fetch_low_guest_instr",
        "static NTSTATUS check_translated_write_access",
    )
    for token in (
        "mutex_lock( &virtual_mutex )",
        "find_view( pc, sizeof(*instr) )",
        "VPROT_ARM64EC",
        "VPROT_SYSTEM",
        "arm64ec_view",
        "VPROT_COMMITTED | VPROT_EXEC | VPROT_GUARD",
        "arm64ec_scalar_load",
        "mutex_unlock( &virtual_mutex )",
    ):
        require(fetch, token)

    write_validation = section(
        virtual_source,
        "static NTSTATUS check_translated_write_access",
        "NTSTATUS virtual_arm64ec_low_guest_access",
    )
    for token in (
        "get_host_page_vprot",
        "get_translated_host_page_vprot",
        "physical_vprot & VPROT_WRITEWATCH",
        "get_unix_prot( translated_vprot ) & PROT_WRITE",
    ):
        require(write_validation, token)

    access = section(
        virtual_source,
        "NTSTATUS virtual_arm64ec_low_guest_access",
        "virtual_setup_exception",
    )
    for token in (
        "mutex_lock( &virtual_mutex )",
        "page_size - ((host + offset) & page_mask)",
        "VPROT_AMD64_LOW_TRANSLATED",
        "VPROT_COMMITTED",
        "VPROT_GUARD",
        "VPROT_EXEC | VPROT_WRITEWATCH",
        "get_unix_prot( vprot ) & PROT_READ",
        "check_translated_write_access",
        "arm64ec_scalar_load",
        "arm64ec_scalar_store",
        "arm64ec_q_load",
        "arm64ec_q_store",
        "arm64ec_qpair_load",
        "arm64ec_qpair_store",
        "arm64ec_gpr_pair_load",
        "arm64ec_gpr_pair_store",
        "mutex_unlock( &virtual_mutex )",
    ):
        require(access, token)

    # Signal-context emulation may inspect metadata and bytes, but V1 observer
    # publication and all VM/write-watch mutation stay in normal context.
    for forbidden in (
        "arm64ec_low_memory_begin_transaction",
        "arm64ec_low_memory_complete_transaction",
        "set_page_vprot",
        "mprotect",
        "update_write_watches",
    ):
        if forbidden in write_validation or forbidden in access:
            raise AssertionError(f"signal bridge mutates VM state through {forbidden}")


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} SIGNAL_ARM64_C VIRTUAL_C", file=sys.stderr)
        return 2
    verify(
        Path(sys.argv[1]).read_text(encoding="utf-8"),
        Path(sys.argv[2]).read_text(encoding="utf-8"),
    )
    print("ARM64EC fixed-low data access integration regression passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
