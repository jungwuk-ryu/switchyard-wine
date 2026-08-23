#!/usr/bin/env python3
"""Source/model regression for ARM64EC fixed-low instruction-cache flushes."""

from pathlib import Path
import re
import sys


SHADOW_BASE = 0x0000010000000000
SHADOW_SIZE = 0x0000000100000000
PAGE_SIZE = 0x1000
HOST_PAGE_SIZE = 0x4000


def function_body(source: str, name: str) -> str:
    match = re.search(
        rf"(?m)^[A-Za-z_][A-Za-z0-9_\s*]*\b{re.escape(name)}\s*\([^;]*?\)\s*\{{",
        source,
        re.DOTALL,
    )
    if not match:
        raise AssertionError(f"missing function {name}")
    start = source.find("{", match.start())
    depth = 0
    for offset in range(start, len(source)):
        if source[offset] == "{":
            depth += 1
        elif source[offset] == "}":
            depth -= 1
            if not depth:
                return source[start : offset + 1]
    raise AssertionError(f"unterminated function {name}")


def require_order(body: str, label: str, *tokens: str) -> None:
    cursor = 0
    for token in tokens:
        offset = body.find(token, cursor)
        if offset < 0:
            raise AssertionError(f"{label} is missing ordered token {token!r}")
        cursor = offset + len(token)


def check_source(source: str) -> None:
    runs = function_body(source, "flush_instruction_cache_accessible_runs")
    committed = function_body(source, "is_instruction_cache_page_committed")
    helper = function_body(source, "flush_arm64ec_low_instruction_cache")
    syscall = function_body(source, "NtFlushInstructionCache")

    require_order(
        runs,
        "physical accessible-run flush",
        "page = start & ~host_page_mask",
        "get_translated_host_page_vprot( (void *)page )",
        "get_host_page_vprot( (void *)page )",
        "get_unix_prot( vprot ) != PROT_NONE",
        "__clear_cache( (char *)run_start, (char *)run_end )",
    )
    require_order(
        committed,
        "logical committed-page validation",
        "view->protect & SEC_RESERVE",
        "page >= *cached_end",
        "get_committed_size( view, (void *)page, end - page",
        "VPROT_COMMITTED",
        "run < page_size",
        "run & page_mask",
        "run > end - page",
        "*cached_end = page + run",
        "return *cached_committed",
    )
    require_order(
        helper,
        "LOW flush overflow validation",
        "guest >= WINE_LOW_VA_SHADOW_SIZE",
        "size > WINE_LOW_VA_SHADOW_SIZE - guest",
        "host = WINE_LOW_VA_SHADOW_BASE + guest",
        "host_end = host + size",
    )
    require_order(
        helper,
        "LOW flush ownership scan and pin",
        "server_enter_uninterrupted_section( &virtual_mutex",
        "find_view( (void *)page, page_size )",
        "view->protect & VPROT_SHADOW_TRANSLATED) ==\n"
        "                 VPROT_AMD64_LOW_TRANSLATED",
        "view->protect & VPROT_SYSTEM",
        "is_instruction_cache_page_committed(",
        "if (any_low || other_translated)",
        "!translated_valid",
        "flush_instruction_cache_accessible_runs( host, host_end, TRUE )",
        "view->protect & (VPROT_SHADOW_TRANSLATED | VPROT_SYSTEM)",
        "flush_instruction_cache_accessible_runs( guest, guest_end, FALSE )",
        "server_leave_uninterrupted_section( &virtual_mutex",
    )
    if "memory_begin_transaction" in helper or "memory_complete_transaction" in helper:
        raise AssertionError("cache-only operation must not publish a LOW memory mutation")
    require_order(
        syscall,
        "LOW flush syscall routing",
        "current_process = handle == NtCurrentProcess()",
        "if (!current_process)",
        "NtCompareObjects( handle, NtCurrentProcess() )",
        "if (current_process)",
        "if (!addr) return STATUS_SUCCESS",
        "is_arm64ec() && addr && size",
        "(ULONG_PTR)addr < WINE_LOW_VA_SHADOW_SIZE",
        "return flush_arm64ec_low_instruction_cache( addr, size )",
        "__clear_cache( (char *)addr, (char *)addr + size )",
    )


def resolve_model(guest, size, shadow_pages, identity_pages):
    """Mirror the address-model decision; None means reject without a clear."""

    if guest >= SHADOW_SIZE or size > SHADOW_SIZE - guest:
        return None
    host = SHADOW_BASE + guest
    end = host + size
    page = host & ~(PAGE_SIZE - 1)
    last_page = (end - 1) & ~(PAGE_SIZE - 1)
    any_low = False
    other_translated = False
    translated_valid = True
    while True:
        owner, committed = shadow_pages.get(page, ("none", False))
        if owner in ("low", "low_system"):
            any_low = True
            if owner == "low_system" or not committed:
                translated_valid = False
        else:
            translated_valid = False
            if owner in ("wow64", "dual"):
                other_translated = True
        if page == last_page:
            break
        page += PAGE_SIZE

    if any_low or other_translated:
        if not any_low or other_translated or not translated_valid:
            return None
        return host, end

    identity_end = guest + size
    page = guest & ~(PAGE_SIZE - 1)
    last_page = (identity_end - 1) & ~(PAGE_SIZE - 1)
    while True:
        owner, committed = identity_pages.get(page, ("none", False))
        if owner != "ordinary" or not committed:
            return None
        if page == last_page:
            return guest, identity_end
        page += PAGE_SIZE


def accessible_runs(start, end, pages):
    page = start & ~(HOST_PAGE_SIZE - 1)
    result = []
    run_start = None
    run_end = None
    while page < end:
        segment_start = max(page, start)
        segment_end = min(page + HOST_PAGE_SIZE, end)
        if pages.get(page, False):
            if run_start is None:
                run_start = segment_start
            run_end = segment_end
        elif run_start is not None:
            result.append((run_start, run_end))
            run_start = run_end = None
        page += HOST_PAGE_SIZE
    if run_start is not None:
        result.append((run_start, run_end))
    return result


def is_current_process_model(exact_pseudo, compare_status):
    return exact_pseudo or compare_status == 0


def null_flush_model(address, size):
    if address is None:
        return "full"
    if not size:
        return "empty"
    return "range"


def check_model() -> None:
    first = SHADOW_BASE + 0x20000000
    shadow = {
        first: ("low", True),
        first + PAGE_SIZE: ("low", True),
    }
    identity = {
        0x20000000: ("ordinary", True),
        0x20001000: ("ordinary", True),
    }

    assert resolve_model(0x20000006, 6, shadow, {}) == (first + 6, first + 12)
    assert resolve_model(0x20000ffe, 4, shadow, {}) == (first + 0xffe, first + 0x1002)
    assert resolve_model(SHADOW_SIZE - 1, 1,
                         {SHADOW_BASE + SHADOW_SIZE - PAGE_SIZE: ("low", True)},
                         {})
    assert resolve_model(SHADOW_SIZE - 1, 2, shadow, identity) is None
    assert resolve_model(0x20000ffe, 4, {first: ("low", True)}, identity) is None
    assert resolve_model(0x20000ffe, 4,
                         {first: ("low", True), first + PAGE_SIZE: ("ordinary", True)},
                         identity) is None
    assert resolve_model(0x20000006, 6, {first: ("wow64", True)}, identity) is None
    assert resolve_model(0x20000006, 6, {first: ("dual", True)}, identity) is None
    assert resolve_model(0x20000006, 6, {first: ("low_system", True)}, identity) is None
    assert resolve_model(0x20000006, 6, {first: ("low", False)}, identity) is None
    assert resolve_model(0x20000ffe, 4, {}, identity) == (0x20000ffe, 0x20001002)
    assert resolve_model(0x20000ffe, 4, {}, {0x20000000: ("ordinary", True)}) is None
    assert resolve_model(0x20000006, 6, {}, {0x20000000: ("system", True)}) is None

    physical = first & ~(HOST_PAGE_SIZE - 1)
    assert accessible_runs(first + 0x1000, first + 0xb000,
                           {physical: True, physical + 0x4000: False,
                            physical + 0x8000: True}) == [
                                (first + 0x1000, physical + 0x4000),
                                (physical + 0x8000, first + 0xb000),
                            ]
    assert accessible_runs(first, first + 0x4000, {physical: False}) == []
    assert is_current_process_model(True, 0xC0000008)
    assert is_current_process_model(False, 0)
    assert not is_current_process_model(False, 0xC0000008)
    assert not is_current_process_model(False, 0xC0000001)
    assert null_flush_model(None, 0) == "full"
    assert null_flush_model(None, 0x1234) == "full"
    assert null_flush_model(0x20000000, 0) == "empty"


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} VIRTUAL_C", file=sys.stderr)
        return 2
    source = Path(sys.argv[1]).read_text(encoding="utf-8")
    check_source(source)
    check_model()

    mutations = (
        source.replace(
            "view->protect & VPROT_SHADOW_TRANSLATED) ==\n"
            "                 VPROT_AMD64_LOW_TRANSLATED",
            "view->protect & VPROT_SHADOW_TRANSLATED) !=\n"
            "                 VPROT_AMD64_LOW_TRANSLATED",
            1,
        ),
        source.replace("get_unix_prot( vprot ) != PROT_NONE",
                       "get_unix_prot( vprot ) == PROT_NONE", 1),
        source.replace("current_process = !NtCompareObjects( handle, NtCurrentProcess() )",
                       "current_process = FALSE", 1),
        source.replace("if (!addr) return STATUS_SUCCESS", "if (!addr) return STATUS_ACCESS_VIOLATION", 1),
        source.replace(
            "server_enter_uninterrupted_section( &virtual_mutex, &sigset );\n\n"
            "    /* Scan the entire shadow interval",
            "/* lock removed */\n\n    /* Scan the entire shadow interval",
            1,
        ),
    )
    for index, mutated in enumerate(mutations):
        if mutated == source:
            raise AssertionError(f"mutation {index} did not change the source")
        try:
            check_source(mutated)
        except AssertionError:
            continue
        raise AssertionError(f"regression checker accepted mutation {index}")

    print("ARM64EC fixed-low instruction-cache contract verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
