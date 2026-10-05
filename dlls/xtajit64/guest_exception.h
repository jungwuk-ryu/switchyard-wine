/*
 * FEX x86-64 guest exception mapping
 *
 * Copyright 2026 Switchyard contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_XTAJIT64_GUEST_EXCEPTION_H
#define __WINE_XTAJIT64_GUEST_EXCEPTION_H

struct xtajit64_guest_exception_mapping
{
    NTSTATUS code;
    UINT32 parameter_count;
    UINT32 clear_trap_flag;
    UINT64 context_rip;
    UINT64 exception_address;
    UINT64 information[2];
};

/* Keep this mapping byte-for-byte compatible with FEX's Windows exception
 * contract.  In particular, breakpoint context RIP and ExceptionAddress are
 * intentionally not interchangeable; anti-debug code observes the difference. */
static inline void xtajit64_map_guest_exception(
    UINT32 signal, UINT32 trap, UINT32 error_code, UINT64 rip,
    UINT64 rax, UINT64 rcx, struct xtajit64_guest_exception_mapping *mapping )
{
    UINT32 interrupt;

    mapping->code = STATUS_ILLEGAL_INSTRUCTION;
    mapping->parameter_count = 0;
    mapping->clear_trap_flag = 0;
    mapping->context_rip = rip;
    mapping->exception_address = rip;
    mapping->information[0] = 0;
    mapping->information[1] = 0;

    switch (signal)
    {
    case 4: /* FEX FAULT_SIGILL */
        return;
    case 5: /* FEX FAULT_SIGTRAP */
        if (trap == 1)
        {
            mapping->code = STATUS_SINGLE_STEP;
            mapping->clear_trap_flag = 1;
        }
        else if (trap == 3)
        {
            mapping->code = STATUS_BREAKPOINT;
            mapping->exception_address = rip - 1;
            mapping->parameter_count = 1;
        }
        return;
    case 11: /* FEX FAULT_SIGSEGV */
        if (trap == 13 && (error_code & 7) == 2)
        {
            interrupt = error_code >> 3;
            switch (interrupt)
            {
            case 3:
                mapping->context_rip += 2;
                mapping->code = STATUS_BREAKPOINT;
                mapping->exception_address = mapping->context_rip - 1;
                mapping->parameter_count = 1;
                return;
            case 0x29:
                mapping->code = STATUS_STACK_BUFFER_OVERRUN;
                mapping->parameter_count = 1;
                mapping->information[0] = rcx;
                return;
            case 0x2c:
                mapping->code = STATUS_ASSERTION_FAILURE;
                return;
            case 0x2d:
                mapping->context_rip += 3;
                mapping->code = STATUS_BREAKPOINT;
                mapping->exception_address = mapping->context_rip;
                mapping->parameter_count = 1;
                mapping->information[0] = rax;
                return;
            default:
                return;
            }
        }
        if (trap == 13)
        {
            mapping->code = STATUS_PRIVILEGED_INSTRUCTION;
            return;
        }
        if (trap == 4)
        {
            mapping->code = STATUS_INTEGER_OVERFLOW;
            return;
        }
        if (trap == 14)
        {
            mapping->code = STATUS_ACCESS_VIOLATION;
            mapping->parameter_count = 2;
            mapping->information[0] = EXCEPTION_EXECUTE_FAULT;
            mapping->information[1] = rip;
        }
        return;
    default:
        return;
    }
}

#endif /* __WINE_XTAJIT64_GUEST_EXCEPTION_H */
