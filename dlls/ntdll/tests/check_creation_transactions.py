#!/usr/bin/env python3
"""Source-model regression for private process/thread creation transactions."""

from pathlib import Path
import re
import sys


def function_body(source: str, name: str) -> str:
    patterns = (
        re.compile(
            rf"(?m)^[A-Za-z_][A-Za-z0-9_\s*]*\b{re.escape(name)}\s*\([^;]*?\)\s*\{{",
            re.DOTALL,
        ),
        re.compile(rf"(?m)^DECL_HANDLER\(\s*{re.escape(name)}\s*\)\s*\{{"),
    )
    match = next((pattern.search(source) for pattern in patterns if pattern.search(source)), None)
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


def require(body: str, label: str, token: str) -> None:
    if token not in body:
        raise AssertionError(f"{label} is missing {token!r}")


def require_order(body: str, label: str, *tokens: str) -> None:
    cursor = 0
    for token in tokens:
        offset = body.find(token, cursor)
        if offset < 0:
            raise AssertionError(f"{label} is missing ordered token {token!r}")
        cursor = offset + len(token)


def verify(process: str, thread: str) -> None:
    process_destroy = function_body(process, "startup_info_destroy")
    require_order(
        process_destroy,
        "process transaction destruction",
        "cancel_startup_transaction( info, STATUS_UNSUCCESSFUL )",
        "if (info->process) release_object( info->process )",
    )
    process_close = function_body(process, "startup_info_close_handle")
    require_order(
        process_close,
        "process transaction final-handle cancellation",
        "if (obj->handle_count == 1)",
        "cancel_startup_transaction( info, STATUS_UNSUCCESSFUL )",
    )
    process_cancel = function_body(process, "cancel_startup_transaction")
    require_order(
        process_cancel,
        "process transaction cancellation state",
        "info->transaction = STARTUP_TRANSACTION_CANCELLED",
        "terminate_process( process, NULL, exit_code )",
        "!process->running_threads && process->startup_state == STARTUP_IN_PROGRESS",
        "cleanup_terminated_process( process )",
    )
    process_complete = function_body(process, "complete_new_process")
    require_order(
        process_complete,
        "process transaction wire validation",
        "req->commit != 0 && req->commit != 1",
        "get_handle_obj( current->process",
    )
    new_process = function_body(process, "new_process")
    require_order(
        new_process,
        "process transaction publication",
        "reply->info = alloc_handle( current->process, info",
        "reply->handle = alloc_handle_no_access_check( current->process, process",
        "process->startup_info = (struct startup_info *)grab_object( info )",
        "info->process = (struct process *)grab_object( process )",
    )
    require(new_process, "process partial-handle rollback", "close_handle( current->process, reply->info )")

    require(thread, "thread transaction state model", "THREAD_STARTUP_TRANSACTION_NONE")
    thread_create = function_body(thread, "create_thread_startup_transaction")
    require_order(
        thread_create,
        "thread transaction publication",
        "info->transaction = THREAD_STARTUP_TRANSACTION_NONE",
        "handle = alloc_handle_no_access_check( current->process, info",
        "if (handle) info->transaction = THREAD_STARTUP_TRANSACTION_ACTIVE",
        "release_object( info )",
    )
    thread_destroy = function_body(thread, "thread_startup_info_destroy")
    require_order(
        thread_destroy,
        "thread transaction destruction",
        "cancel_thread_startup_transaction( info, STATUS_UNSUCCESSFUL )",
        "if (info->thread) release_object( info->thread )",
    )
    thread_close = function_body(thread, "thread_startup_info_close_handle")
    require_order(
        thread_close,
        "thread transaction final-handle cancellation",
        "if (obj->handle_count == 1)",
        "cancel_thread_startup_transaction( info, STATUS_UNSUCCESSFUL )",
    )
    thread_complete = function_body(thread, "complete_new_thread")
    require_order(
        thread_complete,
        "thread transaction wire validation",
        "req->commit != 0 && req->commit != 1",
        "get_handle_obj(",
    )


def require_rejected(process: str, thread: str, label: str) -> None:
    try:
        verify(process, thread)
    except AssertionError:
        return
    raise AssertionError(f"checker accepted mutation: {label}")


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} SERVER_PROCESS_C SERVER_THREAD_C", file=sys.stderr)
        return 2
    process = Path(sys.argv[1]).read_text(encoding="utf-8")
    thread = Path(sys.argv[2]).read_text(encoding="utf-8")
    verify(process, thread)

    mutations = (
        (
            process.replace("req->commit != 0 && req->commit != 1", "req->commit > 1", 1),
            thread,
            "signed process commit validation",
        ),
        (
            process.replace(
                "if (obj->handle_count == 1)\n"
                "        cancel_startup_transaction( info, STATUS_UNSUCCESSFUL )",
                "if (1)\n"
                "        cancel_startup_transaction( info, STATUS_UNSUCCESSFUL )",
                1,
            ),
            thread,
            "process final-handle cancellation",
        ),
        (
            process,
            thread.replace(
                "info->transaction = THREAD_STARTUP_TRANSACTION_NONE",
                "info->transaction = THREAD_STARTUP_TRANSACTION_ACTIVE",
                1,
            ),
            "unpublished thread transaction state",
        ),
    )
    for mutated_process, mutated_thread, label in mutations:
        require_rejected(mutated_process, mutated_thread, label)

    print("process/thread creation transaction source contracts verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
