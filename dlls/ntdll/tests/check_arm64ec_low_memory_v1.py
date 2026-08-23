#!/usr/bin/env python3
"""Source/model regression for the ARM64EC fixed-low memory observer v1."""

from pathlib import Path
import re
import sys


SHADOW_BASE = 0x0000010000000000
SHADOW_SIZE = 0x0000000100000000
PAGE_SIZE = 0x1000


def function_body(source: str, name: str) -> str:
    pattern = re.compile(
        rf"(?m)^[A-Za-z_][A-Za-z0-9_\s*]*\b{re.escape(name)}\s*\([^;]*?\)\s*\{{",
        re.DOTALL,
    )
    match = pattern.search(source)
    if not match:
        pattern = re.compile(
            rf"(?m)^DECL_HANDLER\(\s*{re.escape(name)}\s*\)\s*\{{"
        )
        match = pattern.search(source)
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


def require(body: str, label: str, token: str, count: int = 1) -> None:
    actual = body.count(token)
    if actual < count:
        raise AssertionError(
            f"{label} contains {actual} {token!r} occurrence(s), expected {count}"
        )


def reject(body: str, label: str, token: str) -> None:
    if token in body:
        raise AssertionError(f"{label} unexpectedly contains {token!r}")


def require_order(body: str, label: str, *tokens: str) -> None:
    cursor = 0
    for token in tokens:
        offset = body.find(token, cursor)
        if offset < 0:
            raise AssertionError(f"{label} is missing ordered token {token!r}")
        cursor = offset + len(token)


def numeric_define(source: str, name: str) -> int:
    match = re.search(
        rf"(?m)^#define\s+{re.escape(name)}\s+(0x[0-9a-fA-F]+|[0-9]+)u?(?:ll)?\b",
        source,
    )
    if not match:
        raise AssertionError(f"missing numeric definition {name}")
    return int(match.group(1), 0)


def check_abi(header: str, virtual: str) -> None:
    if numeric_define(header, "WINE_ARM64EC_LOW_MEMORY_OBSERVER_VERSION") != 1:
        raise AssertionError("observer version changed")
    if numeric_define(
        header, "WINE_ARM64EC_LOW_MEMORY_OBSERVER_CAP_EXACT_POST_SNAPSHOT"
    ) != 1:
        raise AssertionError("observer capability changed")
    if numeric_define(header, "WINE_ARM64EC_LOW_MEMORY_RANGE_VALID_FLAGS") != 0:
        raise AssertionError("unknown range flags became valid")
    if numeric_define(header, "WINE_ARM64EC_LOW_MEMORY_EVENT_FULL_SNAPSHOT") != 1:
        raise AssertionError("FULL snapshot flag changed")

    declarations = (
        "struct wine_arm64ec_low_memory_range_v1",
        "struct wine_arm64ec_low_memory_event_v1",
        "struct wine_arm64ec_low_memory_observer_v1",
        "__wine_register_arm64ec_low_memory_observer_v1",
    )
    for declaration in declarations:
        require(header, "public LOW observer ABI", declaration)

    layout_asserts = {
        "wine_arm64ec_low_memory_range_v1": (
            40,
            (
                ("host_address", 0),
                ("size", 8),
                ("host_allocation_base", 16),
                ("state", 24),
                ("protect", 28),
                ("flags", 32),
                ("reserved", 36),
            ),
        ),
        "wine_arm64ec_low_memory_event_v1": (
            72,
            (
                ("version", 0),
                ("size", 4),
                ("operation", 8),
                ("flags", 12),
                ("status", 16),
                ("snapshot_status", 20),
                ("reserved", 24),
                ("host_address", 32),
                ("size_covered", 40),
                ("host_allocation_base", 48),
                ("ranges", 56),
                ("range_count", 64),
            ),
        ),
        "wine_arm64ec_low_memory_observer_v1": (
            40,
            (
                ("version", 0),
                ("size", 4),
                ("context", 8),
                ("begin", 16),
                ("complete", 24),
                ("capabilities", 32),
            ),
        ),
    }
    for struct_name, (size, fields) in layout_asserts.items():
        require(
            virtual,
            f"{struct_name} layout",
            f"C_ASSERT( sizeof(struct {struct_name}) == {size} );",
        )
        for field, offset in fields:
            require(
                virtual,
                f"{struct_name} layout",
                f"offsetof(struct {struct_name}, {field}) == {offset} );",
            )


def check_registration(virtual: str) -> None:
    body = function_body(virtual, "__wine_register_arm64ec_low_memory_observer_v1")
    require(body, "registration", "observer->size != sizeof(*observer)")
    require(
        body,
        "registration",
        "observer->capabilities !=\n            WINE_ARM64EC_LOW_MEMORY_OBSERVER_CAP_EXACT_POST_SNAPSHOT",
    )
    require(body, "registration", "STATUS_ALREADY_REGISTERED")
    require(body, "registration", "if (!is_arm64ec())")
    require_order(
        body,
        "registration ordering",
        "mutex_lock( &arm64ec_low_memory_observer_mutex )",
        "observer->begin(",
        "server_enter_uninterrupted_section( &virtual_mutex",
        "__atomic_store_n( &arm64ec_low_memory_observer_required, TRUE",
        "arm64ec_low_memory_snapshot_range(",
        "arm64ec_low_memory_observer_registered = TRUE",
        "server_leave_uninterrupted_section( &virtual_mutex",
        "observer->complete(",
        "mutex_unlock( &arm64ec_low_memory_observer_mutex )",
    )
    require(
        body,
        "registration rollback",
        "__atomic_store_n( &arm64ec_low_memory_observer_required, FALSE",
    )
    require(body, "initial FULL snapshot", "WINE_ARM64EC_LOW_MEMORY_EVENT_FULL_SNAPSHOT")
    require(body, "initial FULL snapshot", "WINE_LOW_VA_SHADOW_SIZE", 3)


def check_snapshot(virtual: str) -> None:
    body = function_body(virtual, "arm64ec_low_memory_snapshot_range")
    require(body, "snapshot lower bound", "find_view_at_or_after( (void *)address )")
    require(body, "snapshot iteration", "struct file_view *current = view")
    require(body, "snapshot current-view flags", "current->protect")
    require(body, "snapshot owner authority", "VPROT_AMD64_LOW_TRANSLATED")
    require(body, "snapshot FREE gaps", "MEM_FREE", 3)
    require(body, "snapshot FREE protection", "PAGE_NOACCESS", 3)
    require(body, "snapshot exact alignment", "address & page_mask")
    require(body, "snapshot exact alignment", "size & page_mask")
    require(body, "snapshot overflow bound", "size > shadow_end - address")

    append = function_body(virtual, "arm64ec_low_memory_append_range")
    require(append, "range coalescing", "range->host_address + range->size == address")
    require(append, "range coalescing", "range->state == state")
    require(append, "range zeroing", "range->flags = 0")
    require(append, "range zeroing", "range->reserved = 0")

    # Independently exercise the published interval model: gaps and ordinary
    # shadow views remain FREE, while adjacent equal ranges coalesce.
    views = [
        (SHADOW_BASE + 0x2000, 0x2000, False),
        (SHADOW_BASE + 0x4000, 0x3000, True),
        (SHADOW_BASE + 0x9000, 0x1000, True),
    ]
    cursor = SHADOW_BASE
    end = SHADOW_BASE + 0xB000
    ranges: list[tuple[int, int, str]] = []
    for start, size, owned in views:
        if start > cursor:
            ranges.append((cursor, start - cursor, "free"))
        state = "owned" if owned else "free"
        ranges.append((start, size, state))
        cursor = start + size
    if cursor < end:
        ranges.append((cursor, end - cursor, "free"))
    coalesced: list[tuple[int, int, str]] = []
    for start, size, state in ranges:
        if coalesced and coalesced[-1][0] + coalesced[-1][1] == start and coalesced[-1][2] == state:
            old_start, old_size, _ = coalesced[-1]
            coalesced[-1] = (old_start, old_size + size, state)
        else:
            coalesced.append((start, size, state))
    if coalesced[0] != (SHADOW_BASE, 0x4000, "free"):
        raise AssertionError(f"leading/ordinary FREE ranges were not coalesced: {coalesced}")
    if coalesced[-1] != (SHADOW_BASE + 0xA000, 0x1000, "free"):
        raise AssertionError(f"trailing FREE range is missing: {coalesced}")
    if sum(size for _, size, _ in coalesced) != end - SHADOW_BASE:
        raise AssertionError("snapshot model does not cover the requested interval exactly")


def check_transaction_core(virtual: str) -> None:
    normalize = function_body(
        virtual, "arm64ec_low_memory_normalize_begin_interval"
    )
    require(normalize, "transaction begin interval", "if (!size) size = 1")
    require(normalize, "transaction begin interval", "start &= ~page_mask")
    require(normalize, "transaction begin interval", "end = (end + page_mask) & ~page_mask")
    require(normalize, "transaction begin interval", "size > shadow_end - start")
    require(normalize, "transaction begin interval", "end > shadow_end")

    begin = function_body(virtual, "arm64ec_low_memory_begin_transaction")
    require_order(
        begin,
        "transaction begin",
        "if (!candidate) return STATUS_SUCCESS",
        "arm64ec_low_memory_normalize_begin_interval(",
        "if (arm64ec_low_memory_observer_callback_active)",
        "if (arm64ec_low_memory_current_transaction)",
        "transaction->nested = TRUE",
        "WINE_ARM64EC_LOW_MEMORY_EVENT_FULL_SNAPSHOT",
        "mutex_lock( &arm64ec_low_memory_observer_mutex )",
        "transaction->observer->begin(",
        "(ULONG_PTR)begin_address, begin_size",
        "transaction->observer_begun = TRUE",
        "arm64ec_low_memory_current_transaction = transaction",
    )
    complete = function_body(virtual, "arm64ec_low_memory_complete_transaction")
    require_order(
        complete,
        "transaction complete",
        "if (transaction->nested || !transaction->gate_locked) return",
        "arm64ec_low_memory_current_transaction = NULL",
        "if (transaction->observer_begun)",
        "transaction->observer->complete(",
        "mutex_unlock( &arm64ec_low_memory_observer_mutex )",
    )
    capture = function_body(virtual, "arm64ec_low_memory_capture_transaction")
    require(capture, "nested FULL capture", "WINE_ARM64EC_LOW_MEMORY_EVENT_FULL_SNAPSHOT")
    require(capture, "nested FULL capture", "WINE_LOW_VA_SHADOW_SIZE")
    require_order(
        capture,
        "failed unmap/release completion",
        "else if (!size)",
        "transaction->event.host_address",
        "transaction->event.size_covered",
        "arm64ec_low_memory_snapshot_range(",
    )


def check_mutator(virtual: str, name: str) -> None:
    body = function_body(virtual, name)
    require_order(
        body,
        f"{name} transaction",
        "arm64ec_low_memory_begin_transaction(",
        "server_enter_uninterrupted_section( &virtual_mutex",
        "arm64ec_low_memory_capture_transaction(",
        "server_leave_uninterrupted_section( &virtual_mutex",
        "arm64ec_low_memory_complete_transaction(",
    )


def check_mapping_and_codec(virtual: str, server: str, loader: str) -> None:
    require(
        virtual,
        "shared shadow owner mask",
        "#define VPROT_SHADOW_TRANSLATED (VPROT_WOW64_TRANSLATED | VPROT_AMD64_LOW_TRANSLATED)",
    )
    for name in (
        "virtual_map_image",
        "allocate_virtual_memory",
        "NtFreeVirtualMemory",
        "NtProtectVirtualMemory",
        "unmap_view_of_section",
    ):
        check_mutator(virtual, name)

    mapping = function_body(virtual, "map_view")
    require(mapping, "shared shadow owner", "VPROT_SHADOW_TRANSLATED", 6)
    require(mapping, "owner-domain validation", "!is_inside_wow64_shadow( base, size )", 2)
    require(mapping, "dual-owner rejection", "requested_owner == VPROT_SHADOW_TRANSLATED")
    require(mapping, "dual-owner rejection", "(vprot & VPROT_SHADOW_TRANSLATED) == VPROT_SHADOW_TRANSLATED")
    require(mapping, "ordinary shadow rejection", "overlaps_wow64_shadow( base, size )")

    main = function_body(virtual, "virtual_map_module")
    require(
        main,
        "pre-PEB main-image provenance",
        "translated_amd64_low = current_machine == IMAGE_FILE_MACHINE_ARM64",
        2,
    )
    section = re.sub(r"\s+", " ", function_body(virtual, "virtual_map_section"))
    require(
        section,
        "later image negative provenance",
        "translated_wow64, FALSE",
        2,
    )
    image = function_body(virtual, "virtual_map_image")
    for token in (
        "translated_amd64_low && !offset",
        "IMAGE_FILE_MACHINE_AMD64",
        "IMAGE_FILE_DLL",
        "IMAGE_FILE_RELOCS_STRIPPED",
        "pe_mapping->image.base < limit_4g",
        "VPROT_AMD64_LOW_TRANSLATED",
        "IMAGE_VIEW_TRANSLATED_AMD64_LOW",
    ):
        require(image, "fixed-low image provenance", token)

    load_main = function_body(loader, "load_main_exe")
    require(
        load_main,
        "pre-PEB builtin fallback provenance",
        "current_machine == IMAGE_FILE_MACHINE_ARM64",
    )
    require(
        load_main,
        "pre-PEB builtin fallback machine",
        "search_machine == IMAGE_FILE_MACHINE_AMD64",
    )
    reject(load_main, "pre-PEB builtin fallback cycle", "is_arm64ec()")

    allocate = function_body(virtual, "allocate_virtual_memory")
    require(
        allocate,
        "explicit LOW reserve provenance",
        "low_new_reserve = low_input && (type & MEM_RESERVE)",
    )
    require(
        allocate,
        "explicit LOW reserve excludes placeholder replacement",
        "!(type & MEM_REPLACE_PLACEHOLDER)",
    )
    require_order(
        allocate,
        "explicit LOW reserve provider handoff",
        "arm64ec_low_memory_begin_transaction(",
        "server_enter_uninterrupted_section( &virtual_mutex",
        "get_vprot_flags( protect, &vprot, FALSE )",
        "if (low_new_reserve && !arm64ec_low_memory_observer_is_required())",
        "status = STATUS_NOT_SUPPORTED",
        "base = low_capture_base",
        "vprot |= VPROT_AMD64_LOW_TRANSLATED",
        "low_view = TRUE",
        "arm64ec_low_memory_capture_transaction(",
    )

    map_request = function_body(server, "map_image_view")
    for token in (
        "current->process->vm_flags_valid",
        "IMAGE_FILE_MACHINE_AMD64",
        "IMAGE_FILE_DLL",
        "IMAGE_FILE_RELOCS_STRIPPED",
        "req->guest_base != mapping->image.base",
        "req->size > 0x100000000ULL - req->guest_base",
    ):
        require(map_request, "server main-image provenance", token)

    candidate = function_body(virtual, "get_arm64ec_low_candidate_range")
    require(candidate, "canonical-low codec", "WINE_LOW_VA_SHADOW_BASE + value")
    require(candidate, "canonical-low overflow", "size > WINE_LOW_VA_SHADOW_SIZE - value")
    for name in ("NtFreeVirtualMemory", "NtProtectVirtualMemory", "unmap_view_of_section"):
        body = function_body(virtual, name)
        require(body, f"{name} canonical-low codec", "get_arm64ec_low_candidate_range")
        require(body, f"{name} ownership check", "VPROT_AMD64_LOW_TRANSLATED")
        if name != "unmap_view_of_section":
            require(body, f"{name} canonical output", "- WINE_LOW_VA_SHADOW_BASE")
    basic = function_body(virtual, "fill_basic_memory_info")
    require(basic, "basic query canonical lookup", "VPROT_AMD64_LOW_TRANSLATED")
    require(basic, "basic query canonical output", "- WINE_LOW_VA_SHADOW_BASE", 2)
    translated = function_body(virtual, "query_translated_view_information")
    require(translated, "translated query host output", "local.HostBase = region")
    require(translated, "translated query guest output", "WINE_TRANSLATED_VIEW_AMD64_LOW")
    require(translated, "translated query guest output", "- WINE_LOW_VA_SHADOW_BASE", 2)


def check_main_image_dual_base(
    unix_env: str,
    unix_loader: str,
    unix_private: str,
    pe_env: str,
    pe_loader: str,
    actctx: str,
    ntdll_misc: str,
    spec: str,
) -> None:
    init_peb = function_body(unix_env, "init_peb")
    require_order(
        init_peb,
        "main-image PEB guest base",
        "main_image_info.Machine == IMAGE_FILE_MACHINE_AMD64",
        "guest = virtual_get_guest_address( module )",
        "main_image_native_base = guest != module ? module : NULL",
        "module = guest",
        "peb->ImageBaseAddress           = module",
    )
    require(init_peb, "ordinary main-image reset", "else main_image_native_base = NULL")
    require(unix_private, "native main-image declaration", "extern void *main_image_native_base;")

    load_ntdll = function_body(unix_loader, "load_ntdll_functions")
    require_order(
        load_ntdll,
        "main-image native-base handoff",
        "GET_FUNC( __wine_main_image_native_base )",
        "*p__wine_main_image_native_base = main_image_native_base",
    )

    require(pe_loader, "PE main-image native-base storage", "void *__wine_main_image_native_base = NULL;")
    require(pe_loader, "main-image module cache", "static WINE_MODREF *main_image_modref;")
    get_main_client_base = function_body(pe_loader, "get_main_image_client_base")
    require_order(
        get_main_client_base,
        "canonical main-image client base",
        "if (main_image_modref) return main_image_modref->ldr.DllBase",
        "if (!(peb = NtCurrentTeb()->Peb)) return NULL",
        "return peb->ImageBaseAddress",
    )
    get_modref = function_body(pe_loader, "get_modref")
    require_order(
        get_modref,
        "canonical main-module lookup",
        "main_image_modref && hmod == get_main_image_client_base()",
        "return cached_modref = main_image_modref",
    )

    relocations = function_body(pe_loader, "perform_relocations")
    require_order(
        relocations,
        "canonical relocation comparison",
        "base = (char *)nt->OptionalHeader.ImageBase",
        "if (client_base == base) return STATUS_SUCCESS",
        "client_base != get_main_image_client_base()",
        "delta = (char *)client_base - base",
    )
    require(
        relocations,
        "native relocation backing",
        "LdrProcessRelocationBlock( get_rva( module, rel->VirtualAddress )",
    )

    build_module = function_body(pe_loader, "build_module")
    require(
        build_module,
        "module client-base parameter",
        "perform_relocations( *module, client_base, nt, map_size )",
    )
    build_main = function_body(pe_loader, "build_main_module")
    require_order(
        build_main,
        "main module native/client split",
        "client_base = NtCurrentTeb()->Peb->ImageBaseAddress",
        "NtQueryInformationProcess(",
        "info.Machine == IMAGE_FILE_MACHINE_AMD64 && __wine_main_image_native_base",
        "? __wine_main_image_native_base : client_base",
        "convert_to_pe64( module, &info )",
        "build_module( NULL, &nt_name, &module, client_base",
        "return main_image_modref = wm",
    )
    require(
        pe_loader,
        "ordinary module client-base calls",
        "build_module( load_path, nt_name, &module, module, image_info",
    )
    require(
        pe_loader,
        "ordinary builtin client-base calls",
        "load_path, &params.nt_name, &module, module, &image_info",
    )

    wow64_env = function_body(pe_env, "set_wow64_environment")
    require(wow64_env, "deferred main-header read", "USHORT machine;")
    require_order(
        wow64_env,
        "deferred main-header read",
        "if (!is_win64 && NtCurrentTeb()->WowTebOffset)",
        "machine = RtlImageNtHeader( NtCurrentTeb()->Peb->ImageBaseAddress )",
    )
    if wow64_env.count("RtlImageNtHeader( NtCurrentTeb()->Peb->ImageBaseAddress )") != 1:
        raise AssertionError("set_wow64_environment has an eager or duplicate main-header read")

    actctx_init = function_body(actctx, "actctx_init")
    require_order(
        actctx_init,
        "native activation-context parsing",
        "__wine_main_image_native_base ? __wine_main_image_native_base",
        ": NtCurrentTeb()->Peb->ImageBaseAddress",
        "RtlCreateActivationContext(",
    )
    require(ntdll_misc, "PE main-image native-base declaration", "extern void *__wine_main_image_native_base;")
    if spec.rstrip().splitlines()[-1] != "@ extern -private __wine_main_image_native_base":
        raise AssertionError("main-image native-base export is not append-only at the spec tail")


def verify(header: str, virtual: str, server: str, loader: str) -> None:
    check_abi(header, virtual)
    check_registration(virtual)
    check_snapshot(virtual)
    check_transaction_core(virtual)
    check_mapping_and_codec(virtual, server, loader)


def main() -> int:
    if len(sys.argv) != 4:
        print(f"usage: {sys.argv[0]} LOW_VA_H VIRTUAL_C SERVER_MAPPING_C", file=sys.stderr)
        return 2
    header = Path(sys.argv[1]).read_text(encoding="utf-8")
    virtual = Path(sys.argv[2]).read_text(encoding="utf-8")
    server = Path(sys.argv[3]).read_text(encoding="utf-8")
    virtual_path = Path(sys.argv[2])
    unix_dir = virtual_path.parent
    ntdll_dir = unix_dir.parent
    loader = unix_dir.joinpath("loader.c").read_text(encoding="utf-8")
    verify(header, virtual, server, loader)
    dual_base_sources = {
        "unix_env": unix_dir.joinpath("env.c").read_text(encoding="utf-8"),
        "unix_loader": loader,
        "unix_private": unix_dir.joinpath("unix_private.h").read_text(encoding="utf-8"),
        "pe_env": ntdll_dir.joinpath("env.c").read_text(encoding="utf-8"),
        "pe_loader": ntdll_dir.joinpath("loader.c").read_text(encoding="utf-8"),
        "actctx": ntdll_dir.joinpath("actctx.c").read_text(encoding="utf-8"),
        "ntdll_misc": ntdll_dir.joinpath("ntdll_misc.h").read_text(encoding="utf-8"),
        "spec": ntdll_dir.joinpath("ntdll.spec").read_text(encoding="utf-8"),
    }
    check_main_image_dual_base(**dual_base_sources)

    mutations = (
        virtual.replace(
            "#define VPROT_SHADOW_TRANSLATED (VPROT_WOW64_TRANSLATED | VPROT_AMD64_LOW_TRANSLATED)",
            "#define VPROT_SHADOW_TRANSLATED (VPROT_WOW64_TRANSLATED)",
            1,
        ),
        virtual.replace(
            "arm64ec_low_memory_observer_registered = TRUE;",
            "/* registration publication removed */",
            1,
        ),
        virtual.replace(
            "translated_wow64, FALSE );",
            "translated_wow64, TRUE );",
            1,
        ),
    )
    for index, mutated in enumerate(mutations):
        if mutated == virtual:
            raise AssertionError(f"mutation {index} did not change the source")
        try:
            verify(header, mutated, server, loader)
        except AssertionError:
            continue
        raise AssertionError(f"regression checker accepted mutation {index}")

    mutated_loader = dual_base_sources["pe_loader"].replace(
        "build_module( NULL, &nt_name, &module, client_base",
        "build_module( NULL, &nt_name, &module, module",
        1,
    )
    if mutated_loader == dual_base_sources["pe_loader"]:
        raise AssertionError("main client-base mutation did not change the source")
    mutated_sources = dict(dual_base_sources)
    mutated_sources["pe_loader"] = mutated_loader
    try:
        check_main_image_dual_base(**mutated_sources)
    except AssertionError:
        pass
    else:
        raise AssertionError("regression checker accepted native main base as relocation client base")

    print("ARM64EC fixed-low observer v1 core contract verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
