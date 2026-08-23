#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VIRTUAL_SOURCE="$ROOT_DIR/dlls/ntdll/unix/virtual.c"
TEST_ROOT="$(/usr/bin/mktemp -d "${TMPDIR:-/tmp}/darwin-arm64-private-valloc-wx.XXXXXX")"
trap '/bin/rm -rf -- "$TEST_ROOT"' EXIT

fail()
{
    echo "Darwin ARM64 private-valloc W^X source test: $*" >&2
    exit 1
}

[ -f "$VIRTUAL_SOURCE" ] || fail "missing $VIRTUAL_SOURCE"

PYTHONDONTWRITEBYTECODE=1 TEST_CC="${CC:-cc}" /usr/bin/python3 -I - \
    "$VIRTUAL_SOURCE" "$TEST_ROOT" <<'PY'
import os
import pathlib
import re
import shlex
import subprocess
import sys


source_path = pathlib.Path(sys.argv[1])
test_root = pathlib.Path(sys.argv[2])
source = source_path.read_text(encoding="utf-8")


def fail(message: str) -> None:
    raise SystemExit(f"Darwin ARM64 private-valloc W^X source test: {message}")


def strip_comments_and_literals(text: str) -> str:
    """Blank comments and quoted literals while preserving offsets and newlines."""
    result = list(text)
    index = 0
    state = "code"
    quote = ""

    while index < len(text):
        char = text[index]
        next_char = text[index + 1] if index + 1 < len(text) else ""
        if state == "code":
            if char == "/" and next_char == "/":
                result[index] = result[index + 1] = " "
                index += 2
                state = "line-comment"
                continue
            if char == "/" and next_char == "*":
                result[index] = result[index + 1] = " "
                index += 2
                state = "block-comment"
                continue
            if char in ('"', "'"):
                quote = char
                result[index] = " "
                index += 1
                state = "literal"
                continue
        elif state == "line-comment":
            if char == "\n":
                state = "code"
            else:
                result[index] = " "
            index += 1
            continue
        elif state == "block-comment":
            if char == "*" and next_char == "/":
                result[index] = result[index + 1] = " "
                index += 2
                state = "code"
                continue
            if char != "\n":
                result[index] = " "
            index += 1
            continue
        else:
            if char == "\\" and next_char:
                result[index] = " "
                if next_char != "\n":
                    result[index + 1] = " "
                index += 2
                continue
            if char == quote:
                state = "code"
            if char != "\n":
                result[index] = " "
            index += 1
            continue
        index += 1

    if state == "block-comment":
        fail("virtual.c contains an unterminated block comment")
    if state == "literal":
        fail("virtual.c contains an unterminated quoted literal")
    return "".join(result)


clean_source = strip_comments_and_literals(source)


def extract_function(name: str) -> tuple[str, int, int]:
    pattern = re.compile(rf"\b{re.escape(name)}\s*\([^;{{}}]*\)\s*\{{", re.DOTALL)
    match = pattern.search(clean_source)
    if not match:
        fail(f"cannot locate the {name} definition")

    brace = clean_source.find("{", match.start())
    depth = 0
    end = brace
    while end < len(clean_source):
        if clean_source[end] == "{":
            depth += 1
        elif clean_source[end] == "}":
            depth -= 1
            if depth == 0:
                break
        end += 1
    if depth:
        fail(f"cannot find the end of {name}")

    start = clean_source.rfind("\n", 0, match.start()) + 1
    return source[start:end + 1], start, end + 1


def preprocessor_stack_at(offset: int) -> list[str]:
    stack: list[str] = []
    for line in source[:offset].splitlines():
        directive = line.strip()
        match = re.match(r"#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", directive)
        if not match:
            continue
        kind, condition = match.group(1), match.group(2).strip()
        if kind == "if":
            stack.append(condition)
        elif kind == "ifdef":
            stack.append(f"defined({condition})")
        elif kind == "ifndef":
            stack.append(f"!defined({condition})")
        elif kind == "elif":
            if not stack:
                fail("unbalanced #elif while inspecting virtual.c")
            stack[-1] = condition
        elif kind == "else":
            if not stack:
                fail("unbalanced #else while inspecting virtual.c")
            stack[-1] = f"!({stack[-1]})"
        else:
            if not stack:
                fail("unbalanced #endif while inspecting virtual.c")
            stack.pop()
    return stack


def has_apple_arm64_guard(offset: int) -> bool:
    for condition in preprocessor_stack_at(offset):
        compact = re.sub(r"\s+", "", condition)
        if ("defined(__APPLE__)" in compact and
                "defined(__aarch64__)" in compact and
                "&&" in compact and "||" not in compact and
                "!defined(__APPLE__)" not in compact and
                "!defined(__aarch64__)" not in compact):
            return True
    return False


mprotect_exec_source, _, _ = extract_function("mprotect_exec")
view_bounds_source, view_bounds_start, _ = extract_function("get_mprotect_view_bounds")
next_view_source, _, _ = extract_function("next_mprotect_view")
can_retry_source, can_retry_start, _ = extract_function("can_retry_native_writable_exec")
retry_source, retry_start, retry_end = extract_function("mprotect_range_run")
translated_projection_source, _, _ = extract_function("get_mprotect_translated_host_page_vprot")
domain_source, _, _ = extract_function("mprotect_range_domain")
validate_domains_source, validate_domains_start, _ = extract_function("validate_mprotect_domains")
mprotect_source, mprotect_start, _ = extract_function("mprotect_range")
validate_memory_access_source, _, _ = extract_function("validate_mprotect_memory_access_range")
memory_access_source, _, _ = extract_function("mprotect_memory_access_range")
native_fault_source, _, _ = extract_function("virtual_handle_fault")
wow64_fault_source, _, _ = extract_function("__wine_resolve_wow64_memory_fault_v1")
grow_stack_source, _, _ = extract_function("grow_thread_stack")
setup_exception_source, _, _ = extract_function("virtual_setup_exception")
allocate_source, _, _ = extract_function("allocate_virtual_memory")
map_view_source, _, _ = extract_function("map_view")
snapshot_source, _, _ = extract_function("snapshot_vprot")
restore_source, _, _ = extract_function("restore_vprot_or_abort")
restore_uniform_source, _, _ = extract_function("restore_uniform_vprot_or_abort")

grow_stack_clean = strip_comments_and_literals(grow_stack_source)
wow64_fault_clean = strip_comments_and_literals(wow64_fault_source)
setup_exception_clean = strip_comments_and_literals(setup_exception_source)
if re.search(r"\b(?:malloc|calloc|realloc|free|snapshot_vprot)\s*\(", grow_stack_clean):
    fail("stack-fault growth allocates memory on the fault path")
if re.search(r"\bprotect_failed\b", grow_stack_clean + wow64_fault_clean):
    fail("stack-fault protection failure is published out-of-band")
if (not re.search(r"\bSTATUS_ACCESS_DENIED\b", grow_stack_clean) or
        not re.search(r"\brestore_vprot_or_abort\s*\(", grow_stack_clean)):
    fail("stack-fault growth does not fail closed with exact vprot rollback")
if not re.search(r"\bSTATUS_ACCESS_DENIED\b", wow64_fault_clean):
    fail("the WoW64 fault bridge does not reject failed stack reprotection")
if (not re.search(r"\bSTATUS_ACCESS_DENIED\b", setup_exception_clean) or
        not re.search(r"\babort_thread\s*\(", setup_exception_clean)):
    fail("exception-stack setup can continue after failed stack reprotection")

if not has_apple_arm64_guard(can_retry_start):
    fail("the private-valloc eligibility helper is not gated to Apple ARM64")
if not has_apple_arm64_guard(view_bounds_start):
    fail("the physical view-bounds helper is not gated to Apple ARM64")
if not has_apple_arm64_guard(validate_domains_start):
    fail("the physical ownership-domain validator is not gated to Apple ARM64")

retry_clean = clean_source[retry_start:retry_end]
retry_gate = retry_clean.find("errno")
if retry_gate < 0 or not has_apple_arm64_guard(retry_start + retry_gate):
    fail("the writable-executable retry is not gated to Apple ARM64")

# MAP_JIT is allowed in rationale comments, but not in executable source.
if re.search(r"\bMAP_JIT\b", clean_source):
    fail("virtual.c implements MAP_JIT instead of isolating private valloc from the JIT domain")

mprotect_exec_clean = strip_comments_and_literals(mprotect_exec_source)
if not re.search(
        r"if\s*\(\s*cpu_provider_owned\s*\)\s*unix_prot\s*&=\s*~PROT_EXEC\s*;",
        mprotect_exec_clean,
    ):
    fail("CPU-provider-owned domains can retain host execute permission")

can_retry_clean = strip_comments_and_literals(can_retry_source)
if (re.search(r"\bfind_view\s*\(", can_retry_clean) or
        not re.search(r"\bget_mprotect_view_bounds\s*\(", can_retry_clean) or
        not re.search(r"\bpage_end\s*=\s*\(const char \*\)min\s*\(\s*end\s*,\s*view_end\s*\)",
                      can_retry_clean)):
    fail("private-valloc retry does not use its exact owner and logical tail bound")

domain_clean = strip_comments_and_literals(domain_source)
if (not re.search(r"\bis_shadow_translated_vprot\s*\(\s*view\s*->\s*protect\s*\)",
                  domain_clean) or
        not re.search(r"\bview\s*->\s*protect\s*&\s*VPROT_CPU_PROVIDER_OWNED\b",
                      domain_clean)):
    fail("physical domains do not retain their translated/provider ownership policy")

translated_projection_clean = strip_comments_and_literals(translated_projection_source)
delta_projection = translated_projection_clean.find(
    "page_vprot = (page_vprot & ~clear) | set"
)
guard_filter = translated_projection_clean.find("page_vprot & VPROT_GUARD")
if (delta_projection < 0 or guard_filter < 0 or delta_projection > guard_filter or
        not re.search(
            r"!delegated\s*&&\s*\(\s*clear\s*&\s*VPROT_WRITEWATCH\s*\)",
            translated_projection_clean,
        ) or
        not re.search(r"page\s*<\s*logical_end\s*&&\s*"
                      r"page\s*\+\s*page_size\s*>\s*logical_start",
                      translated_projection_clean)):
    fail("translated aggregation does not project lane deltas before filtering")

validate_domains_clean = strip_comments_and_literals(validate_domains_source)
if (not re.search(r"\bfind_view_at_or_after\s*\(", validate_domains_clean) or
        not re.search(r"\bget_mprotect_view_bounds\s*\(", validate_domains_clean) or
        not re.search(r"\bnext_mprotect_view\s*\(", validate_domains_clean) or
        not re.search(r"\berrno\s*=\s*EINVAL\s*;", validate_domains_clean)):
    fail("physical ownership domains are not validated before protection changes")

mprotect_clean = strip_comments_and_literals(mprotect_source)
validate_call = mprotect_clean.find("validate_mprotect_domains")
apply_call = mprotect_clean.find("mprotect_range_domain")
if (validate_call < 0 or apply_call < 0 or validate_call > apply_call or
        not re.search(r"if\s*\(\s*!size\s*\)\s*return\s+0\s*;", mprotect_clean) or
        not re.search(r"logical_end\s*>\s*~\(ULONG_PTR\)0\s*-\s*host_page_mask",
                      mprotect_clean)):
    fail("mprotect_range does not validate its complete domain walk before applying it")

memory_access_clean = strip_comments_and_literals(memory_access_source)
if (not re.search(r"\boverlaps_wow64_shadow\s*\(", memory_access_clean) or
        not re.search(r"\bvalidate_mprotect_memory_access_range\s*\(",
                      memory_access_clean)):
    fail("native-copy reprotection no longer validates translated shadow ownership")

validate_memory_access_clean = strip_comments_and_literals(validate_memory_access_source)
if (not re.search(r"size\s*>\s*~\(ULONG_PTR\)0\s*-\s*address",
                  validate_memory_access_clean) or
        not re.search(
            r"\(\s*view\s*->\s*protect\s*&\s*VPROT_CPU_PROVIDER_OWNED\s*\)\s*"
            r"!=\s*VPROT_WOW64_TRANSLATED",
            validate_memory_access_clean,
        ) or
        not re.search(r"\bget_mprotect_view_bounds\s*\(",
                      validate_memory_access_clean)):
    fail("shadow reprotection preflight accepts gaps or ambiguous CPU ownership")

fault_contracts = (
    ("native", native_fault_source, "VPROT_GUARD", "page", "host_page_size"),
    ("native", native_fault_source, "VPROT_WRITEWATCH", "page", "host_page_size"),
    ("WoW64", wow64_fault_source, "VPROT_GUARD", "logical_page", "page_size"),
    ("WoW64", wow64_fault_source, "VPROT_WRITEWATCH", "clear_base", "clear_size"),
)
for fault_name, fault_source, bit, base, size in fault_contracts:
    fault_clean = strip_comments_and_literals(fault_source)
    protect_match = re.search(
        rf"mprotect_range\s*\(\s*{base}\s*,\s*{size}\s*,\s*0\s*,\s*{bit}\s*\)",
        fault_clean,
    )
    clear_match = re.search(
        rf"set_page_vprot_bits\s*\([^;]*,\s*0\s*,\s*{bit}\s*\)",
        fault_clean,
    )
    if not protect_match or not clear_match or protect_match.start() > clear_match.start():
        fail(f"{fault_name} fault publishes the {bit} clear before physical success")

allocate_clean = strip_comments_and_literals(allocate_source)


def matching_delimiter(text: str, start: int, opening: str, closing: str) -> int:
    if start >= len(text) or text[start] != opening:
        return -1
    depth = 0
    for offset in range(start, len(text)):
        if text[offset] == opening:
            depth += 1
        elif text[offset] == closing:
            depth -= 1
            if not depth:
                return offset
    return -1


def enclosing_if_block(text: str, offset: int):
    for match in reversed(list(re.finditer(r"\bif\s*\(", text[:offset]))):
        open_paren = text.find("(", match.start())
        close_paren = matching_delimiter(text, open_paren, "(", ")")
        if close_paren < offset:
            continue
        block_start = close_paren + 1
        while block_start < len(text) and text[block_start].isspace():
            block_start += 1
        if block_start >= len(text) or text[block_start] != "{":
            continue
        block_end = matching_delimiter(text, block_start, "{", "}")
        if block_end < 0:
            continue
        return text[open_paren + 1:close_paren], text[block_start + 1:block_end], block_end
    return None


mprotect_call = re.search(
    r"\bmprotect_range\s*\(\s*base\s*,\s*size\s*,\s*0\s*,\s*0\s*\)",
    allocate_clean,
)
failure_block = enclosing_if_block(allocate_clean, mprotect_call.start()) if mprotect_call else None
if not failure_block:
    fail("initial executable mprotect failure does not delete the view and set STATUS_ACCESS_DENIED")
failure_condition, failure_body, failure_end = failure_block
if (not re.search(r"\bvprot\s*&\s*VPROT_EXEC\b", failure_condition) or
        not re.search(r"\bforce_exec_prot\b", failure_condition)):
    fail("initial executable mprotect is not gated by executable protection")
if not re.search(
        r"!\s*\(\s*type\s*&\s*MEM_REPLACE_PLACEHOLDER\s*\)",
        failure_condition):
    fail("new-view mprotect cleanup does not exclude the atomic placeholder replacement path")
if not re.search(
        r"&&\s*mprotect_range\s*\(\s*base\s*,\s*size\s*,\s*0\s*,\s*0\s*\)"
        r"\s*(?:!=\s*0)?\s*$", failure_condition):
    fail("initial executable mprotect failure is not propagated")

cleanup_statements = (
    r"\bdelete_view\s*\(\s*view\s*\)\s*;",
    r"\bview\s*=\s*NULL\s*;",
    r"\bstatus\s*=\s*STATUS_ACCESS_DENIED\s*;",
)
cleanup_offset = 0
for statement in cleanup_statements:
    match = re.search(statement, failure_body[cleanup_offset:])
    if not match:
        fail("initial executable mprotect failure does not delete the view and set STATUS_ACCESS_DENIED")
    cleanup_offset += match.end()
if not re.search(r"\breturn\s+status\s*;", allocate_clean[failure_end + 1:]):
    fail("allocate_virtual_memory does not propagate its failure status")

snapshot_clean = strip_comments_and_literals(snapshot_source)
snapshot_contracts = (
    r"\bpage_count\s*=\s*size\s*>>\s*page_shift\s*;",
    r"\bmalloc\s*\(\s*page_count\s*\)",
    r"\bi\s*<\s*page_count\b",
    r"\bsnapshot\s*\[\s*i\s*\]\s*=\s*get_page_vprot\s*\(",
)
if any(not re.search(contract, snapshot_clean) for contract in snapshot_contracts):
    fail("protection rollback does not preserve every logical page's vprot")

restore_clean = strip_comments_and_literals(restore_source)
if (not re.search(r"\bset_page_vprot\s*\(", restore_clean) or
        not re.search(
            r"if\s*\(\s*mprotect_range\s*\([^;{}]*\)\s*\)\s*\{[^{}]*"
            r"abort_process\s*\(\s*STATUS_ACCESS_DENIED\s*\)\s*;",
            restore_clean,
            re.DOTALL,
        )):
    fail("protection rollback does not restore logical and host protection atomically")

restore_uniform_clean = strip_comments_and_literals(restore_uniform_source)
if (not re.search(
        r"\bset_page_vprot\s*\(\s*base\s*,\s*size\s*,\s*vprot\s*\)\s*;",
        restore_uniform_clean,
    ) or
        not re.search(
            r"if\s*\(\s*mprotect_range\s*\([^;{}]*\)\s*\)\s*\{[^{}]*"
            r"abort_process\s*\(\s*STATUS_ACCESS_DENIED\s*\)\s*;",
            restore_uniform_clean,
            re.DOTALL,
        )):
    fail("uniform rollback does not restore logical and host protection atomically")

map_view_clean = strip_comments_and_literals(map_view_source)
placeholder_token = re.search(r"\bMEM_REPLACE_PLACEHOLDER\b", map_view_clean)
placeholder_block = (
    enclosing_if_block(map_view_clean, placeholder_token.start()) if placeholder_token else None
)
if not placeholder_block:
    fail("map_view does not provide a MEM_REPLACE_PLACEHOLDER transaction")
placeholder_condition, placeholder_body, _ = placeholder_block
if not re.search(
        r"\balloc_type\s*&\s*MEM_REPLACE_PLACEHOLDER\b", placeholder_condition):
    fail("map_view placeholder transaction is not gated by MEM_REPLACE_PLACEHOLDER")

placeholder_contracts = (
    r"\bold_protect\s*=\s*view\s*->\s*protect\s*;",
    r"\bview\s*->\s*protect\s*=\s*vprot\s*\|\s*VPROT_PLACEHOLDER\s*;",
)
if any(not re.search(contract, placeholder_body) for contract in placeholder_contracts):
    fail("map_view placeholder transaction does not preserve view metadata")
placeholder_snapshot_contracts = (
    r"\bsnapshot_vprot\s*\(",
    r"\b(?:malloc|calloc|realloc|free)\s*\(",
    r"\bstack_snapshot\b",
    r"\bold_vprot\b",
)
if any(re.search(contract, placeholder_body) for contract in placeholder_snapshot_contracts):
    fail("map_view placeholder transaction allocates or retains a redundant protection snapshot")

placeholder_set = re.search(
    r"\bset_vprot\s*\(\s*view\s*,\s*base\s*,\s*size\s*,\s*vprot\s*\)",
    placeholder_body,
)
placeholder_failure = (
    enclosing_if_block(placeholder_body, placeholder_set.start()) if placeholder_set else None
)
if not placeholder_failure:
    fail("map_view placeholder transaction does not check set_vprot failure")
set_condition, rollback_body, rollback_end = placeholder_failure
if not re.search(r"!\s*set_vprot\s*\(", set_condition):
    fail("map_view placeholder transaction does not enter rollback on set_vprot failure")

rollback_contracts = (
    r"\bview\s*->\s*protect\s*=\s*old_protect\s*;",
    r"\brestore_uniform_vprot_or_abort\s*\(\s*base\s*,\s*size\s*,\s*0\s*\)\s*;",
    r"\breturn\s+STATUS_ACCESS_DENIED\s*;",
)
rollback_offset = 0
for contract in rollback_contracts:
    match = re.search(contract, rollback_body[rollback_offset:])
    if not match:
        fail("map_view placeholder failure does not restore metadata and uniform free-placeholder protection")
    rollback_offset += match.end()
placeholder_success = placeholder_body[rollback_end + 1:]
if (not re.search(r"\*\s*view_ret\s*=\s*view\s*;", placeholder_success) or
        not re.search(r"\breturn\s+STATUS_SUCCESS\s*;", placeholder_success)):
    fail("map_view placeholder success does not publish the replacement view")


def compile_and_run(name: str, program: str) -> None:
    compiler = shlex.split(os.environ.get("TEST_CC", "cc"))
    if not compiler:
        fail("the host C compiler command is empty")
    binary = test_root / name
    command = compiler + [
        "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-Wdeclaration-after-statement", "-Wstrict-prototypes",
        "-x", "c", "-", "-o", str(binary),
    ]
    build = subprocess.run(command, input=program, text=True, capture_output=True)
    if build.returncode:
        fail(f"{name} model failed to compile:\n{build.stderr}")
    run = subprocess.run([str(binary)], text=True, capture_output=True)
    if run.returncode:
        fail(f"{name} model failed with status {run.returncode}:\n{run.stdout}{run.stderr}")


eligibility_model = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef unsigned char BYTE;
typedef int BOOL;
typedef uintptr_t ULONG_PTR;
typedef size_t SIZE_T;

#define FALSE 0
#define TRUE 1
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define VPROT_COMMITTED         0x01
#define VPROT_GUARD             0x02
#define VPROT_EXEC              0x04
#define VPROT_WRITE             0x08
#define VPROT_WRITECOPY         0x10
#define VPROT_SYSTEM            0x20
#define VPROT_SHADOW_TRANSLATED 0x40
#define VPROT_READ              0x80
#define VPROT_AMD64_IDENTITY    0x100
#define VPROT_CPU_PROVIDER_OWNED (VPROT_SHADOW_TRANSLATED | VPROT_AMD64_IDENTITY)

struct file_view
{
    void *base;
    size_t size;
    unsigned int protect;
    BOOL valloc;
};

static const size_t page_size = 4096;
static const ULONG_PTR page_mask = 4095;
static const ULONG_PTR host_page_mask = 16383;
_Alignas(16384) static unsigned char probe_memory[8 * 4096];
static BYTE page_vprot[8];
static size_t page_reads;

static BOOL is_view_valloc(const struct file_view *view)
{
    return view->valloc;
}

static BYTE get_page_vprot(const void *page)
{
    size_t index = ((const unsigned char *)page - probe_memory) / page_size;

    page_reads++;
    if (index >= 8) return 0;
    return page_vprot[index];
}
''' + view_bounds_source + can_retry_source + r'''

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "eligibility check failed at line %d: %s\n", \
                __LINE__, #expression); \
        return 1; \
    } \
} while (0)

static void reset_pages(struct file_view *view)
{
    size_t index;

    memset(page_vprot, 0, sizeof(page_vprot));
    for (index = 0; index < 5; index++)
        page_vprot[index] = VPROT_COMMITTED | VPROT_READ | VPROT_WRITE | VPROT_EXEC;
    page_reads = 0;
    view->base = probe_memory;
    view->size = 5 * page_size;
}

int main(void)
{
    struct file_view view = {probe_memory, 5 * 4096, 0, TRUE};

    reset_pages(&view);
    CHECK(can_retry_native_writable_exec(&view, probe_memory,
                                         sizeof(probe_memory),
                                         (ULONG_PTR)probe_memory,
                                         (ULONG_PTR)probe_memory + sizeof(probe_memory),
                                         0, 0));
    CHECK(page_reads == 5);

    reset_pages(&view);
    CHECK(can_retry_native_writable_exec(&view, probe_memory + 4 * page_size,
                                         4 * page_size,
                                         (ULONG_PTR)probe_memory + 4 * page_size,
                                         (ULONG_PTR)probe_memory + 5 * page_size,
                                         0, 0));
    CHECK(page_reads == 1);

    reset_pages(&view);
    page_vprot[4] = VPROT_COMMITTED | VPROT_READ | VPROT_EXEC;
    CHECK(!can_retry_native_writable_exec(&view, probe_memory,
                                          sizeof(probe_memory),
                                          (ULONG_PTR)probe_memory,
                                          (ULONG_PTR)probe_memory + sizeof(probe_memory),
                                          0, 0));
    CHECK(page_reads == 5);

    reset_pages(&view);
    CHECK(!can_retry_native_writable_exec(&view, probe_memory + 5 * page_size,
                                          page_size,
                                          (ULONG_PTR)probe_memory + 5 * page_size,
                                          (ULONG_PTR)probe_memory + 6 * page_size,
                                          0, 0));
    CHECK(page_reads == 0);

    reset_pages(&view);
    view.valloc = FALSE;
    CHECK(!can_retry_native_writable_exec(&view, probe_memory,
                                          sizeof(probe_memory),
                                          (ULONG_PTR)probe_memory,
                                          (ULONG_PTR)probe_memory + sizeof(probe_memory),
                                          0, 0));
    CHECK(page_reads == 0);
    view.valloc = TRUE;

    reset_pages(&view);
    view.protect = VPROT_SYSTEM;
    CHECK(!can_retry_native_writable_exec(&view, probe_memory,
                                          sizeof(probe_memory),
                                          (ULONG_PTR)probe_memory,
                                          (ULONG_PTR)probe_memory + sizeof(probe_memory),
                                          0, 0));
    CHECK(page_reads == 0);

    reset_pages(&view);
    view.protect = VPROT_AMD64_IDENTITY;
    CHECK(!can_retry_native_writable_exec(&view, probe_memory,
                                          sizeof(probe_memory),
                                          (ULONG_PTR)probe_memory,
                                          (ULONG_PTR)probe_memory + sizeof(probe_memory),
                                          0, 0));
    CHECK(page_reads == 0);

    view.protect = 0;
    reset_pages(&view);
    CHECK(!can_retry_native_writable_exec(&view, probe_memory + 4 * page_size,
                                          4 * page_size,
                                          (ULONG_PTR)probe_memory + 4 * page_size,
                                          (ULONG_PTR)probe_memory + 5 * page_size,
                                          0, VPROT_WRITE));
    CHECK(page_reads == 1);
    return 0;
}
'''


retry_model = r'''
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

typedef unsigned char BYTE;
typedef int BOOL;
typedef uintptr_t ULONG_PTR;

#define FALSE 0
#define TRUE 1
#define PROT_READ  0x01
#define PROT_WRITE 0x02
#define PROT_EXEC  0x04
#define PROT_OTHER 0x08

struct file_view
{
    int unused;
};

static int initial_result;
static int initial_errno;
static int initial_calls;
static int eligibility_calls;
static BOOL eligible;
static int retry_result;
static int retry_calls;
static void *retry_base;
static size_t retry_size;
static int retry_prot;
static ULONG_PTR observed_logical_start;
static ULONG_PTR observed_logical_end;
static BYTE observed_set;
static BYTE observed_clear;

int mprotect_exec(void *base, size_t size, int prot, BOOL translated)
{
    (void)base;
    (void)size;
    (void)prot;
    (void)translated;
    initial_calls++;
    errno = initial_errno;
    return initial_result;
}

BOOL can_retry_native_writable_exec(const struct file_view *view, const void *base,
                                    size_t size, ULONG_PTR logical_start,
                                    ULONG_PTR logical_end, BYTE set, BYTE clear)
{
    (void)view;
    (void)base;
    (void)size;
    eligibility_calls++;
    observed_logical_start = logical_start;
    observed_logical_end = logical_end;
    observed_set = set;
    observed_clear = clear;
    return eligible;
}

int test_mprotect(void *base, size_t size, int prot)
{
    retry_calls++;
    retry_base = base;
    retry_size = size;
    retry_prot = prot;
    return retry_result;
}

#define mprotect test_mprotect
#ifndef __APPLE__
# define __APPLE__ 1
#endif
#ifndef __aarch64__
# define __aarch64__ 1
#endif
''' + retry_source + r'''
#undef mprotect

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "retry check failed at line %d: %s\n", \
                __LINE__, #expression); \
        return 1; \
    } \
} while (0)

static void reset_retry(void)
{
    initial_result = -1;
    initial_errno = EACCES;
    initial_calls = 0;
    eligibility_calls = 0;
    eligible = TRUE;
    retry_result = 0;
    retry_calls = 0;
    retry_base = NULL;
    retry_size = 0;
    retry_prot = 0;
    observed_logical_start = 0;
    observed_logical_end = 0;
    observed_set = 0;
    observed_clear = 0;
}

int main(void)
{
    struct file_view view = {0};
    unsigned char memory[64];
    int prot = PROT_READ | PROT_WRITE | PROT_EXEC | PROT_OTHER;
    int result;

    reset_retry();
    initial_result = 0;
    result = mprotect_range_run(&view, memory, sizeof(memory), prot, FALSE,
                                (ULONG_PTR)memory, (ULONG_PTR)memory + sizeof(memory),
                                0x10, 0x20);
    CHECK(result == 0 && initial_calls == 1);
    CHECK(eligibility_calls == 0 && retry_calls == 0);

    reset_retry();
    initial_errno = EINVAL;
    result = mprotect_range_run(&view, memory, sizeof(memory), prot, FALSE,
                                (ULONG_PTR)memory, (ULONG_PTR)memory + sizeof(memory),
                                0x10, 0x20);
    CHECK(result == -1);
    CHECK(eligibility_calls == 0 && retry_calls == 0);

    reset_retry();
    result = mprotect_range_run(&view, memory, sizeof(memory),
                                PROT_READ | PROT_EXEC, FALSE,
                                (ULONG_PTR)memory, (ULONG_PTR)memory + sizeof(memory),
                                0x10, 0x20);
    CHECK(result == -1);
    CHECK(eligibility_calls == 0 && retry_calls == 0);

    reset_retry();
    result = mprotect_range_run(&view, memory, sizeof(memory),
                                PROT_READ | PROT_WRITE, FALSE,
                                (ULONG_PTR)memory, (ULONG_PTR)memory + sizeof(memory),
                                0x10, 0x20);
    CHECK(result == -1);
    CHECK(eligibility_calls == 0 && retry_calls == 0);

    reset_retry();
    eligible = FALSE;
    result = mprotect_range_run(&view, memory, sizeof(memory), prot, FALSE,
                                (ULONG_PTR)memory, (ULONG_PTR)memory + sizeof(memory),
                                0x10, 0x20);
    CHECK(result == -1);
    CHECK(eligibility_calls == 1 && retry_calls == 0);

    reset_retry();
    result = mprotect_range_run(&view, memory, sizeof(memory), prot, FALSE,
                                (ULONG_PTR)memory, (ULONG_PTR)memory + sizeof(memory),
                                0x10, 0x20);
    CHECK(result == 0);
    CHECK(eligibility_calls == 1 && retry_calls == 1);
    CHECK(retry_base == memory && retry_size == sizeof(memory));
    CHECK(retry_prot == (prot & ~PROT_EXEC));
    CHECK(observed_logical_start == (ULONG_PTR)memory);
    CHECK(observed_logical_end == (ULONG_PTR)memory + sizeof(memory));
    CHECK(observed_set == 0x10 && observed_clear == 0x20);

    reset_retry();
    retry_result = -1;
    result = mprotect_range_run(&view, memory, sizeof(memory), prot, FALSE,
                                (ULONG_PTR)memory, (ULONG_PTR)memory + sizeof(memory),
                                0x10, 0x20);
    CHECK(result == -1 && retry_calls == 1);
    return 0;
}
'''


domain_model = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char BYTE;
typedef int BOOL;
typedef uintptr_t ULONG_PTR;
typedef size_t SIZE_T;

#define FALSE 0
#define TRUE 1
#define min(a, b) ((a) < (b) ? (a) : (b))
#define PROT_NONE  0
#define PROT_READ  0x01
#define PROT_WRITE 0x02
#define PROT_EXEC  0x04
#define VPROT_READ       0x01
#define VPROT_WRITE      0x02
#define VPROT_EXEC       0x04
#define VPROT_WRITECOPY  0x08
#define VPROT_GUARD      0x10
#define VPROT_COMMITTED  0x20
#define VPROT_WRITEWATCH 0x40
#define VPROT_SYSTEM     0x0200
#define VPROT_WOW64_TRANSLATED 0x1000
#define VPROT_AMD64_LOW_TRANSLATED 0x2000
#define VPROT_AMD64_IDENTITY 0x4000
#define VPROT_SHADOW_TRANSLATED \
    (VPROT_WOW64_TRANSLATED | VPROT_AMD64_LOW_TRANSLATED)
#define VPROT_CPU_PROVIDER_OWNED \
    (VPROT_SHADOW_TRANSLATED | VPROT_AMD64_IDENTITY)
#define STATUS_ACCESS_DENIED 0xc0000022u
#define ROUND_ADDR(addr, mask) ((void *)((ULONG_PTR)(addr) & ~(ULONG_PTR)(mask)))
#define WINE_RB_ENTRY_VALUE(entry, type, field) \
    ((type *)((char *)(entry) - offsetof(type, field)))
#ifndef __APPLE__
# define __APPLE__ 1
#endif
#ifndef __aarch64__
# define __aarch64__ 1
#endif

static const size_t page_size = 4096;
static const ULONG_PTR page_mask = 4095;
static const size_t host_page_size = 16384;
static const ULONG_PTR host_page_mask = 16383;

struct wine_rb_entry
{
    struct wine_rb_entry *next;
    struct wine_rb_entry *prev;
};

struct file_view
{
    struct wine_rb_entry entry;
    void *base;
    size_t size;
    unsigned int protect;
};

struct protection_call
{
    struct file_view *view;
    void *base;
    size_t size;
    int requested_prot;
    int effective_prot;
    BOOL cpu_provider_owned;
};

_Alignas(16384) static unsigned char probe_memory[16 * 4096];
static BYTE page_vprot[16];
static struct file_view views[3];
static size_t view_count;
static struct protection_call calls[8];
static size_t call_count;
static size_t lookup_calls;
static BOOL delegated;

static struct wine_rb_entry *rb_next(const struct wine_rb_entry *entry)
{
    return entry->next;
}

static struct wine_rb_entry *rb_prev(const struct wine_rb_entry *entry)
{
    return entry->prev;
}

static BOOL is_shadow_translated_vprot(unsigned int vprot)
{
    return !!(vprot & VPROT_SHADOW_TRANSLATED);
}

static BOOL wow64_memory_logical_write_fault_is_delegated(void)
{
    return delegated;
}

static BYTE get_page_vprot(const void *address)
{
    ULONG_PTR value = (ULONG_PTR)address;
    ULONG_PTR start = (ULONG_PTR)probe_memory;
    size_t index;

    if (value < start || value >= start + sizeof(probe_memory)) return 0;
    index = (value - start) / page_size;
    return page_vprot[index];
}

static BYTE get_host_page_vprot(const void *address)
{
    ULONG_PTR base = (ULONG_PTR)ROUND_ADDR(address, host_page_mask);
    BYTE vprot = 0;
    size_t offset;

    for (offset = 0; offset < host_page_size; offset += page_size)
        vprot |= get_page_vprot((void *)(base + offset));
    return vprot;
}

static int get_unix_prot(BYTE vprot)
{
    int prot = 0;

    if ((vprot & VPROT_COMMITTED) && !(vprot & VPROT_GUARD))
    {
        if (vprot & VPROT_READ) prot |= PROT_READ;
        if (vprot & (VPROT_WRITE | VPROT_WRITECOPY)) prot |= PROT_READ | PROT_WRITE;
        if (vprot & VPROT_EXEC) prot |= PROT_READ | PROT_EXEC;
        if (vprot & VPROT_WRITEWATCH) prot &= ~PROT_WRITE;
    }
    return prot;
}

static struct file_view *find_view_at_or_after(const void *address)
{
    ULONG_PTR value = (ULONG_PTR)address;
    size_t index;

    lookup_calls++;
    for (index = 0; index < view_count; index++)
    {
        ULONG_PTR start = (ULONG_PTR)views[index].base;

        if (views[index].size <= ~(ULONG_PTR)0 - start &&
            start + views[index].size <= value) continue;
        return &views[index];
    }
    return NULL;
}

static int mprotect_range_run(struct file_view *view, void *base, size_t size,
                              int unix_prot, BOOL cpu_provider_owned,
                              ULONG_PTR logical_start, ULONG_PTR logical_end,
                              BYTE set, BYTE clear)
{
    struct protection_call *call;

    (void)logical_start;
    (void)logical_end;
    (void)set;
    (void)clear;
    assert(call_count < sizeof(calls) / sizeof(calls[0]));
    call = &calls[call_count++];
    call->view = view;
    call->base = base;
    call->size = size;
    call->requested_prot = unix_prot;
    call->effective_prot = cpu_provider_owned ? unix_prot & ~PROT_EXEC : unix_prot;
    call->cpu_provider_owned = cpu_provider_owned;
    return 0;
}

static _Noreturn void abort_process(unsigned int status)
{
    (void)status;
    abort();
}

static void set_views(size_t count)
{
    size_t index;

    view_count = count;
    for (index = 0; index < count; index++)
    {
        views[index].entry.prev = index ? &views[index - 1].entry : NULL;
        views[index].entry.next = index + 1 < count ? &views[index + 1].entry : NULL;
    }
}

static void reset_model(void)
{
    memset(page_vprot, 0, sizeof(page_vprot));
    memset(views, 0, sizeof(views));
    memset(calls, 0, sizeof(calls));
    view_count = 0;
    call_count = 0;
    lookup_calls = 0;
    delegated = FALSE;
    errno = 0;
}
''' + view_bounds_source + next_view_source + translated_projection_source + \
    domain_source + validate_domains_source + mprotect_source + r'''

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "domain check failed at line %d: %s\n", \
                __LINE__, #expression); \
        return 1; \
    } \
} while (0)

int main(void)
{
    ULONG_PTR start = (ULONG_PTR)probe_memory;
    size_t index;

    reset_model();
    views[0].base = probe_memory;
    views[0].size = 5 * page_size;
    views[0].protect = VPROT_AMD64_IDENTITY;
    set_views(1);
    page_vprot[4] = VPROT_COMMITTED | VPROT_READ | VPROT_WRITE | VPROT_EXEC;
    CHECK(!mprotect_range(probe_memory + 4 * page_size, page_size, 0, 0));
    CHECK(call_count == 1 && calls[0].view == &views[0]);
    CHECK(calls[0].base == probe_memory + 4 * page_size);
    CHECK(calls[0].size == host_page_size && calls[0].cpu_provider_owned);
    CHECK(calls[0].requested_prot & PROT_EXEC);
    CHECK((calls[0].effective_prot & (PROT_WRITE | PROT_EXEC)) == PROT_WRITE);

    reset_model();
    views[0].base = probe_memory;
    views[0].size = host_page_size;
    views[1].base = probe_memory + host_page_size;
    views[1].size = 5 * page_size;
    views[1].protect = VPROT_AMD64_IDENTITY;
    set_views(2);
    for (index = 0; index < 8; index++)
        page_vprot[index] = VPROT_COMMITTED | VPROT_READ | VPROT_EXEC;
    CHECK(!mprotect_range(probe_memory + 3 * page_size, 2 * page_size, 0, 0));
    CHECK(call_count == 2);
    CHECK(calls[0].view == &views[0] && !calls[0].cpu_provider_owned);
    CHECK(calls[1].view == &views[1] && calls[1].cpu_provider_owned);
    CHECK(calls[0].base == probe_memory && calls[0].size == host_page_size);
    CHECK(calls[1].base == probe_memory + host_page_size &&
          calls[1].size == host_page_size);

    reset_model();
    views[0].base = probe_memory + host_page_size;
    views[0].size = 5 * page_size;
    views[0].protect = VPROT_AMD64_IDENTITY;
    set_views(1);
    CHECK(!mprotect_range(probe_memory, 4 * host_page_size, 0, 0));
    CHECK(call_count == 3);
    CHECK(!calls[0].view && calls[0].base == probe_memory);
    CHECK(calls[1].view == &views[0]);
    CHECK(!calls[2].view && calls[2].base == probe_memory + 3 * host_page_size);

    reset_model();
    views[0].base = probe_memory;
    views[0].size = host_page_size;
    views[0].protect = VPROT_WOW64_TRANSLATED;
    set_views(1);
    page_vprot[0] = VPROT_COMMITTED | VPROT_READ | VPROT_GUARD;
    page_vprot[1] = VPROT_COMMITTED | VPROT_EXEC | VPROT_GUARD;
    CHECK(!mprotect_range(probe_memory, page_size, 0, VPROT_GUARD));
    CHECK(call_count == 1 && (calls[0].requested_prot & PROT_READ));
    CHECK(!(calls[0].requested_prot & PROT_EXEC));

    reset_model();
    views[0].base = probe_memory;
    views[0].size = host_page_size;
    views[0].protect = VPROT_WOW64_TRANSLATED;
    set_views(1);
    page_vprot[0] = VPROT_COMMITTED | VPROT_READ | VPROT_WRITE | VPROT_WRITEWATCH;
    page_vprot[1] = page_vprot[0];
    CHECK(!mprotect_range(probe_memory, page_size, 0, VPROT_WRITEWATCH));
    CHECK(call_count == 1 && (calls[0].requested_prot & PROT_WRITE));

    reset_model();
    views[0].base = probe_memory;
    views[0].size = host_page_size;
    views[0].protect = VPROT_WOW64_TRANSLATED;
    set_views(1);
    page_vprot[0] = VPROT_COMMITTED | VPROT_READ | VPROT_GUARD;
    page_vprot[1] = VPROT_COMMITTED | VPROT_WRITE | VPROT_GUARD;
    CHECK(!mprotect_range(probe_memory, host_page_size, 0, VPROT_GUARD));
    CHECK(call_count == 1 && (calls[0].requested_prot & PROT_WRITE));

    reset_model();
    views[0].base = probe_memory;
    views[0].size = 5 * page_size;
    views[1].base = probe_memory + host_page_size;
    views[1].size = host_page_size;
    set_views(2);
    CHECK(mprotect_range(probe_memory, host_page_size, 0, 0) == -1);
    CHECK(errno == EINVAL && call_count == 0);

    reset_model();
    CHECK(!mprotect_range(probe_memory, 0, 0, 0));
    CHECK(!lookup_calls && !call_count);
    CHECK(mprotect_range((void *)(~(ULONG_PTR)0 - page_mask),
                         2 * page_size, 0, 0) == -1);
    CHECK(errno == EINVAL && !lookup_calls && !call_count);

    reset_model();
    CHECK(mprotect_range((void *)(~(ULONG_PTR)0 - 2 * page_mask),
                         page_size, 0, 0) == -1);
    CHECK(errno == EINVAL && !lookup_calls && !call_count);
    CHECK(start == (ULONG_PTR)probe_memory);
    return 0;
}
'''


uniform_rollback_model = r'''
#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>

typedef unsigned char BYTE;

#define STATUS_ACCESS_DENIED ((unsigned int)0xc0000022)
#define ERR(...) ((void)0)

static unsigned char probe_memory[64];
static jmp_buf abort_environment;
static int event_step;
static int ordering_error;
static int page_vprot_calls;
static void *page_vprot_base;
static size_t page_vprot_size;
static BYTE page_vprot_value;
static int mprotect_calls;
static void *mprotect_base;
static size_t mprotect_size;
static BYTE mprotect_set;
static BYTE mprotect_clear;
static int mprotect_result;
static int abort_calls;
static unsigned int abort_status;

static void set_page_vprot(const void *base, size_t size, BYTE vprot)
{
    if (event_step != 0) ordering_error = 1;
    event_step = 1;
    page_vprot_calls++;
    page_vprot_base = (void *)base;
    page_vprot_size = size;
    page_vprot_value = vprot;
}

static int mprotect_range(void *base, size_t size, BYTE set, BYTE clear)
{
    if (event_step != 1 || page_vprot_calls != 1) ordering_error = 1;
    event_step = 2;
    mprotect_calls++;
    mprotect_base = base;
    mprotect_size = size;
    mprotect_set = set;
    mprotect_clear = clear;
    return mprotect_result;
}

static void abort_process(unsigned int status)
{
    if (event_step != 2 || mprotect_calls != 1) ordering_error = 1;
    event_step = 3;
    abort_calls++;
    abort_status = status;
    longjmp(abort_environment, 1);
}
''' + restore_uniform_source + r'''

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "uniform rollback check failed at line %d: %s\n", \
                __LINE__, #expression); \
        return 1; \
    } \
} while (0)

static void reset_rollback(void)
{
    event_step = 0;
    ordering_error = 0;
    page_vprot_calls = 0;
    page_vprot_base = NULL;
    page_vprot_size = 0;
    page_vprot_value = 0xff;
    mprotect_calls = 0;
    mprotect_base = NULL;
    mprotect_size = 0;
    mprotect_set = 0xff;
    mprotect_clear = 0xff;
    mprotect_result = 0;
    abort_calls = 0;
    abort_status = 0;
}

int main(void)
{
    reset_rollback();
    restore_uniform_vprot_or_abort(probe_memory, sizeof(probe_memory), 0);
    CHECK(!ordering_error && event_step == 2);
    CHECK(page_vprot_calls == 1 && page_vprot_base == probe_memory);
    CHECK(page_vprot_size == sizeof(probe_memory) && page_vprot_value == 0);
    CHECK(mprotect_calls == 1 && mprotect_base == probe_memory);
    CHECK(mprotect_size == sizeof(probe_memory));
    CHECK(mprotect_set == 0 && mprotect_clear == 0 && abort_calls == 0);

    reset_rollback();
    mprotect_result = -1;
    if (!setjmp(abort_environment))
    {
        restore_uniform_vprot_or_abort(probe_memory, sizeof(probe_memory), 0);
        CHECK(0);
    }
    CHECK(!ordering_error && event_step == 3);
    CHECK(page_vprot_calls == 1 && page_vprot_value == 0);
    CHECK(mprotect_calls == 1);
    CHECK(abort_calls == 1 && abort_status == STATUS_ACCESS_DENIED);
    return 0;
}
'''


stack_growth_model = r'''
#include <setjmp.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef unsigned char BYTE;
typedef int BOOL;
typedef uint32_t NTSTATUS;
typedef uintptr_t ULONG_PTR;
typedef size_t SIZE_T;

#define FALSE 0
#define TRUE 1
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define ROUND_SIZE(addr, size, mask) \
    (((SIZE_T)(size) + ((ULONG_PTR)(addr) & (mask)) + (mask)) & ~(ULONG_PTR)(mask))
#define STATUS_SUCCESS ((NTSTATUS)0x00000000)
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xc0000022)
#define STATUS_STACK_OVERFLOW ((NTSTATUS)0xc00000fd)
#define VPROT_COMMITTED 0x01
#define VPROT_GUARD 0x02
#define VPROT_READ 0x04
#define VPROT_WRITE 0x08
#define VPROT_EXEC 0x10
#define VPROT_WRITEWATCH 0x20
#define VPROT_STACK_SNAPSHOT_PAGES 64
#define ERR(...) ((void)0)

static const SIZE_T page_size = 4096;
static const unsigned int page_shift = 12;
static const SIZE_T host_page_size = 16384;

struct model_tib
{
    void *StackLimit;
};

struct model_tib32
{
    ULONG_PTR StackLimit;
};

typedef struct
{
    struct model_tib Tib;
} TEB;

typedef struct
{
    struct model_tib32 Tib;
} WOW_TEB;

struct thread_data
{
    TEB *teb;
};

struct thread_stack_info
{
    char *start;
    char *limit;
    char *end;
    SIZE_T guaranteed;
    BOOL is_wow;
};

struct protect_call
{
    void *base;
    SIZE_T size;
    BYTE set;
    BYTE clear;
    BYTE observed[16];
};

_Alignas(16384) static unsigned char probe_memory[16 * 4096];
static BYTE page_vprot[16];
static struct protect_call protect_calls[8];
static unsigned int protect_call_count;
static unsigned int fail_mask;
static unsigned int abort_count;
static NTSTATUS abort_status;
static jmp_buf abort_environment;
static TEB native_teb;
static WOW_TEB wow_teb;
static struct thread_data thread_data = {&native_teb};

static size_t page_index(const void *address)
{
    const unsigned char *page = address;

    if (page < probe_memory || page >= probe_memory + sizeof(probe_memory) ||
        (size_t)(page - probe_memory) % page_size)
    {
        fprintf(stderr, "invalid model page %p\n", address);
        return ARRAY_SIZE(page_vprot);
    }
    return (size_t)(page - probe_memory) / page_size;
}

static BYTE get_page_vprot(const void *address)
{
    size_t index = page_index(address);

    return index < ARRAY_SIZE(page_vprot) ? page_vprot[index] : 0;
}

static void set_page_vprot(const void *base, SIZE_T size, BYTE vprot)
{
    size_t index = page_index(base);
    size_t count = size / page_size;
    size_t i;

    if (index > ARRAY_SIZE(page_vprot) || count > ARRAY_SIZE(page_vprot) - index) return;
    for (i = 0; i < count; i++) page_vprot[index + i] = vprot;
}

static void set_page_vprot_bits(const void *base, SIZE_T size, BYTE set, BYTE clear)
{
    size_t index = page_index(base);
    size_t count = size / page_size;
    size_t i;

    if (index > ARRAY_SIZE(page_vprot) || count > ARRAY_SIZE(page_vprot) - index) return;
    for (i = 0; i < count; i++)
        page_vprot[index + i] = (page_vprot[index + i] & ~clear) | set;
}

static int mprotect_range(void *base, SIZE_T size, BYTE set, BYTE clear)
{
    struct protect_call *call;

    if (protect_call_count >= ARRAY_SIZE(protect_calls)) return -1;
    call = &protect_calls[protect_call_count++];
    call->base = base;
    call->size = size;
    call->set = set;
    call->clear = clear;
    memcpy(call->observed, page_vprot, sizeof(call->observed));
    return !!(fail_mask & (1u << protect_call_count)) ? -1 : 0;
}

static void abort_process(NTSTATUS status)
{
    abort_count++;
    abort_status = status;
    longjmp(abort_environment, 1);
}

static WOW_TEB *get_wow_teb(TEB *teb)
{
    (void)teb;
    return &wow_teb;
}

static ULONG_PTR wow64_native_to_guest_addr(const void *address)
{
    return (ULONG_PTR)address;
}
''' + restore_source + grow_stack_source + r'''

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "stack growth check failed at line %d: %s\n", \
                __LINE__, #expression); \
        return 1; \
    } \
} while (0)

static void reset_stack_model(struct thread_stack_info *info)
{
    memset(page_vprot, 0, sizeof(page_vprot));
    memset(protect_calls, 0, sizeof(protect_calls));
    protect_call_count = 0;
    fail_mask = 0;
    abort_count = 0;
    abort_status = 0;
    native_teb.Tib.StackLimit = probe_memory + 14 * page_size;
    wow_teb.Tib.StackLimit = (ULONG_PTR)(probe_memory + 13 * page_size);
    info->start = (char *)probe_memory;
    info->limit = (char *)(probe_memory + 4 * page_size);
    info->end = (char *)(probe_memory + 12 * page_size);
    info->guaranteed = page_size;
    info->is_wow = TRUE;
}

static void set_normal_pages(struct thread_stack_info *info, BYTE *before)
{
    reset_stack_model(info);
    page_vprot[0] = VPROT_READ;
    page_vprot[1] = VPROT_READ | VPROT_WRITE;
    page_vprot[2] = VPROT_READ | VPROT_WRITEWATCH;
    page_vprot[3] = VPROT_COMMITTED | VPROT_GUARD | VPROT_READ |
                    VPROT_WRITE | VPROT_EXEC;
    page_vprot[4] = VPROT_COMMITTED | VPROT_READ;
    memcpy(before, page_vprot, sizeof(page_vprot));
}

static int check_normal_success(void)
{
    struct thread_stack_info info;
    BYTE before[ARRAY_SIZE(page_vprot)];
    BYTE expected[ARRAY_SIZE(page_vprot)];
    NTSTATUS status;

    set_normal_pages(&info, before);
    memcpy(expected, before, sizeof(expected));
    expected[2] |= VPROT_COMMITTED | VPROT_GUARD;
    expected[3] = (expected[3] & ~VPROT_GUARD) | VPROT_COMMITTED;
    status = grow_thread_stack(&thread_data, (char *)(probe_memory + 3 * page_size), &info);
    CHECK(status == STATUS_SUCCESS);
    CHECK(protect_call_count == 2);
    CHECK(protect_calls[0].base == probe_memory + 2 * page_size);
    CHECK(protect_calls[0].size == page_size);
    CHECK(protect_calls[0].set == (VPROT_COMMITTED | VPROT_GUARD));
    CHECK(!protect_calls[0].clear);
    CHECK(!(protect_calls[0].observed[2] & VPROT_GUARD));
    CHECK(protect_calls[0].observed[3] & VPROT_GUARD);
    CHECK(protect_calls[1].base == probe_memory + 3 * page_size);
    CHECK(protect_calls[1].size == page_size);
    CHECK(protect_calls[1].set == VPROT_COMMITTED);
    CHECK(protect_calls[1].clear == VPROT_GUARD);
    CHECK(protect_calls[1].observed[2] & VPROT_GUARD);
    CHECK(protect_calls[1].observed[3] & VPROT_GUARD);
    CHECK(!memcmp(page_vprot, expected, sizeof(expected)));
    CHECK(wow_teb.Tib.StackLimit == (ULONG_PTR)(probe_memory + 3 * page_size));
    CHECK(native_teb.Tib.StackLimit == probe_memory + 14 * page_size);
    CHECK(!abort_count);
    return 0;
}

static int check_normal_failure(unsigned int failed_call, unsigned int expected_calls)
{
    struct thread_stack_info info;
    BYTE before[ARRAY_SIZE(page_vprot)];
    ULONG_PTR old_limit;
    NTSTATUS status;

    set_normal_pages(&info, before);
    old_limit = wow_teb.Tib.StackLimit;
    fail_mask = 1u << failed_call;
    status = grow_thread_stack(&thread_data, (char *)(probe_memory + 3 * page_size), &info);
    CHECK(status == STATUS_ACCESS_DENIED);
    CHECK(protect_call_count == expected_calls);
    CHECK(!memcmp(page_vprot, before, sizeof(before)));
    CHECK(wow_teb.Tib.StackLimit == old_limit);
    CHECK(!abort_count);
    if (failed_call == 2)
        CHECK(protect_calls[1].observed[2] & VPROT_GUARD);
    CHECK(protect_calls[expected_calls - 1].set == 0);
    CHECK(protect_calls[expected_calls - 1].clear == 0);
    return 0;
}

static int check_overflow(BOOL fail_protect)
{
    struct thread_stack_info info;
    BYTE before[ARRAY_SIZE(page_vprot)];
    ULONG_PTR old_limit;
    NTSTATUS status;

    reset_stack_model(&info);
    page_vprot[0] = VPROT_READ;
    page_vprot[1] = VPROT_COMMITTED | VPROT_GUARD | VPROT_READ | VPROT_WRITE;
    page_vprot[2] = VPROT_COMMITTED | VPROT_READ;
    memcpy(before, page_vprot, sizeof(before));
    old_limit = wow_teb.Tib.StackLimit;
    if (fail_protect) fail_mask = 1u << 1;
    status = grow_thread_stack(&thread_data, (char *)(probe_memory + page_size), &info);
    CHECK(protect_calls[0].base == probe_memory + page_size);
    CHECK(protect_calls[0].size == page_size);
    CHECK(protect_calls[0].set == VPROT_COMMITTED);
    CHECK(protect_calls[0].clear == VPROT_GUARD);
    CHECK(protect_calls[0].observed[1] & VPROT_GUARD);
    if (fail_protect)
    {
        CHECK(status == STATUS_ACCESS_DENIED);
        CHECK(protect_call_count == 2);
        CHECK(!memcmp(page_vprot, before, sizeof(before)));
        CHECK(wow_teb.Tib.StackLimit == old_limit);
        CHECK(!protect_calls[1].set && !protect_calls[1].clear);
    }
    else
    {
        CHECK(status == STATUS_STACK_OVERFLOW);
        CHECK(protect_call_count == 1);
        CHECK(page_vprot[1] == ((before[1] & ~VPROT_GUARD) | VPROT_COMMITTED));
        CHECK(wow_teb.Tib.StackLimit == (ULONG_PTR)(probe_memory + page_size));
    }
    CHECK(!abort_count);
    return 0;
}

static int check_rollback_abort(void)
{
    struct thread_stack_info info;
    BYTE before[ARRAY_SIZE(page_vprot)];
    ULONG_PTR old_limit;

    set_normal_pages(&info, before);
    old_limit = wow_teb.Tib.StackLimit;
    fail_mask = (1u << 1) | (1u << 2);
    if (!setjmp(abort_environment))
    {
        grow_thread_stack(&thread_data, (char *)(probe_memory + 3 * page_size), &info);
        CHECK(0);
    }
    CHECK(abort_count == 1 && abort_status == STATUS_ACCESS_DENIED);
    CHECK(protect_call_count == 2);
    CHECK(!memcmp(page_vprot, before, sizeof(before)));
    CHECK(wow_teb.Tib.StackLimit == old_limit);
    return 0;
}

int main(void)
{
    if (check_normal_success()) return 1;
    if (check_normal_failure(1, 2)) return 1;
    if (check_normal_failure(2, 3)) return 1;
    if (check_overflow(FALSE)) return 1;
    if (check_overflow(TRUE)) return 1;
    if (check_rollback_abort()) return 1;
    return 0;
}
'''


compile_and_run("private-valloc-eligibility-model", eligibility_model)
compile_and_run("writable-exec-retry-model", retry_model)
compile_and_run("physical-ownership-domain-model", domain_model)
compile_and_run("uniform-protection-rollback-model", uniform_rollback_model)
compile_and_run("stack-growth-atomicity-model", stack_growth_model)
PY

echo "Darwin ARM64 private-valloc W^X source contract verified"
