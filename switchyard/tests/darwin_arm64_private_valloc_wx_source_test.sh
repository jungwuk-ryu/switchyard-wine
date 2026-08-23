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


can_retry_source, can_retry_start, _ = extract_function("can_retry_native_writable_exec")
retry_source, retry_start, retry_end = extract_function("mprotect_range_run")
allocate_source, _, _ = extract_function("allocate_virtual_memory")
map_view_source, _, _ = extract_function("map_view")
snapshot_source, _, _ = extract_function("snapshot_vprot")
restore_source, _, _ = extract_function("restore_vprot_or_abort")
restore_uniform_source, _, _ = extract_function("restore_uniform_vprot_or_abort")

if not has_apple_arm64_guard(can_retry_start):
    fail("the private-valloc eligibility helper is not gated to Apple ARM64")

retry_clean = clean_source[retry_start:retry_end]
retry_gate = retry_clean.find("errno")
if retry_gate < 0 or not has_apple_arm64_guard(retry_start + retry_gate):
    fail("the writable-executable retry is not gated to Apple ARM64")

# MAP_JIT is allowed in rationale comments, but not in executable source.
if re.search(r"\bMAP_JIT\b", clean_source):
    fail("virtual.c implements MAP_JIT instead of isolating private valloc from the JIT domain")

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
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef unsigned char BYTE;
typedef int BOOL;

#define FALSE 0
#define TRUE 1
#define VPROT_COMMITTED         0x01
#define VPROT_GUARD             0x02
#define VPROT_EXEC              0x04
#define VPROT_WRITE             0x08
#define VPROT_WRITECOPY         0x10
#define VPROT_SYSTEM            0x20
#define VPROT_SHADOW_TRANSLATED 0x40
#define VPROT_READ              0x80

struct file_view
{
    BYTE protect;
    BOOL valloc;
};

static const size_t page_size = 4096;
static unsigned char probe_memory[4 * 4096];
static BYTE page_vprot[4];
static size_t page_reads;
static int foreign_page;
static struct file_view foreign_view;
static struct file_view *active_view;

static BOOL is_view_valloc(const struct file_view *view)
{
    return view->valloc;
}

static BYTE get_page_vprot(const void *page)
{
    size_t index = ((const unsigned char *)page - probe_memory) / page_size;

    page_reads++;
    if (index >= 4) return 0;
    return page_vprot[index];
}

static struct file_view *find_view(const void *page, size_t size)
{
    size_t index = ((const unsigned char *)page - probe_memory) / page_size;

    (void)size;
    if ((int)index == foreign_page) return &foreign_view;
    return active_view;
}
''' + can_retry_source + r'''

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
    for (index = 0; index < 4; index++)
        page_vprot[index] = VPROT_COMMITTED | VPROT_READ | VPROT_WRITE | VPROT_EXEC;
    page_reads = 0;
    foreign_page = -1;
    active_view = view;
}

int main(void)
{
    struct file_view view = {0, TRUE};

    reset_pages(&view);
    CHECK(can_retry_native_writable_exec(&view, probe_memory,
                                         sizeof(probe_memory), 0, 0));
    CHECK(page_reads == 4);

    reset_pages(&view);
    page_vprot[3] = VPROT_COMMITTED | VPROT_READ | VPROT_EXEC;
    CHECK(!can_retry_native_writable_exec(&view, probe_memory,
                                          sizeof(probe_memory), 0, 0));
    CHECK(page_reads == 4);

    reset_pages(&view);
    foreign_page = 3;
    CHECK(!can_retry_native_writable_exec(&view, probe_memory,
                                          sizeof(probe_memory), 0, 0));
    CHECK(page_reads == 4);

    reset_pages(&view);
    view.valloc = FALSE;
    CHECK(!can_retry_native_writable_exec(&view, probe_memory,
                                          sizeof(probe_memory), 0, 0));
    CHECK(page_reads == 0);
    view.valloc = TRUE;

    reset_pages(&view);
    view.protect = VPROT_SYSTEM;
    CHECK(!can_retry_native_writable_exec(&view, probe_memory,
                                          sizeof(probe_memory), 0, 0));
    CHECK(page_reads == 0);

    reset_pages(&view);
    view.protect = VPROT_SHADOW_TRANSLATED;
    CHECK(!can_retry_native_writable_exec(&view, probe_memory,
                                          sizeof(probe_memory), 0, 0));
    CHECK(page_reads == 0);

    view.protect = 0;
    reset_pages(&view);
    CHECK(!can_retry_native_writable_exec(&view, probe_memory,
                                          sizeof(probe_memory), 0, VPROT_WRITE));
    return 0;
}
'''


retry_model = r'''
#include <errno.h>
#include <stddef.h>
#include <stdio.h>

typedef unsigned char BYTE;
typedef int BOOL;

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
                                    size_t size, BYTE set, BYTE clear)
{
    (void)view;
    (void)base;
    (void)size;
    eligibility_calls++;
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
    result = mprotect_range_run(&view, memory, sizeof(memory), prot, FALSE, 0x10, 0x20);
    CHECK(result == 0 && initial_calls == 1);
    CHECK(eligibility_calls == 0 && retry_calls == 0);

    reset_retry();
    initial_errno = EINVAL;
    result = mprotect_range_run(&view, memory, sizeof(memory), prot, FALSE, 0x10, 0x20);
    CHECK(result == -1);
    CHECK(eligibility_calls == 0 && retry_calls == 0);

    reset_retry();
    result = mprotect_range_run(&view, memory, sizeof(memory),
                                PROT_READ | PROT_EXEC, FALSE, 0x10, 0x20);
    CHECK(result == -1);
    CHECK(eligibility_calls == 0 && retry_calls == 0);

    reset_retry();
    result = mprotect_range_run(&view, memory, sizeof(memory),
                                PROT_READ | PROT_WRITE, FALSE, 0x10, 0x20);
    CHECK(result == -1);
    CHECK(eligibility_calls == 0 && retry_calls == 0);

    reset_retry();
    eligible = FALSE;
    result = mprotect_range_run(&view, memory, sizeof(memory), prot, FALSE, 0x10, 0x20);
    CHECK(result == -1);
    CHECK(eligibility_calls == 1 && retry_calls == 0);

    reset_retry();
    result = mprotect_range_run(&view, memory, sizeof(memory), prot, FALSE, 0x10, 0x20);
    CHECK(result == 0);
    CHECK(eligibility_calls == 1 && retry_calls == 1);
    CHECK(retry_base == memory && retry_size == sizeof(memory));
    CHECK(retry_prot == (prot & ~PROT_EXEC));
    CHECK(observed_set == 0x10 && observed_clear == 0x20);

    reset_retry();
    retry_result = -1;
    result = mprotect_range_run(&view, memory, sizeof(memory), prot, FALSE, 0x10, 0x20);
    CHECK(result == -1 && retry_calls == 1);
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


compile_and_run("private-valloc-eligibility-model", eligibility_model)
compile_and_run("writable-exec-retry-model", retry_model)
compile_and_run("uniform-protection-rollback-model", uniform_rollback_model)
PY

echo "Darwin ARM64 private-valloc W^X source contract verified"
