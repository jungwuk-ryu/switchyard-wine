/*
 * General Windows CONTEXT contract: edited PF/AF/DF survive VEH continuation.
 *
 * Copyright 2026 Switchyard Wine contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

static uint64_t *fault_page;
static DWORD desired_flags, handler_owner;
static volatile LONG faults;
extern unsigned char softflag_store[];
extern uint64_t flag_fault(uint64_t *);

__asm__(".text\n"
        ".p2align 4\n"
        ".globl flag_fault\n"
        "flag_fault:\n"
        "movq $10,%rax\n"
        "subq $1,%rax\n"
        ".globl softflag_store\n"
        "softflag_store:\n"
        "movq %rax,(%rcx)\n"
        "pushfq\n"
        "popq %rax\n"
        "cld\n"
        "ret\n");

static LONG WINAPI handler(EXCEPTION_POINTERS *pointers)
{
    EXCEPTION_RECORD *rec=pointers->ExceptionRecord;
    CONTEXT *context=pointers->ContextRecord;
    DWORD old;
    if (!fault_page || GetCurrentThreadId()!=handler_owner ||
        rec->ExceptionCode!=EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters<2 ||
        rec->ExceptionInformation[0]!=1 ||
        rec->ExceptionInformation[1]!=(ULONG_PTR)fault_page ||
        context->Rip!=(DWORD64)(uintptr_t)softflag_store ||
        context->Rax!=9 || context->Rcx!=(DWORD64)(uintptr_t)fault_page ||
        (context->EFlags&0xcd5u)!=4 || faults)
        return EXCEPTION_CONTINUE_SEARCH;
    if (!VirtualProtect(fault_page,4096,PAGE_READWRITE,&old))
        return EXCEPTION_CONTINUE_SEARCH;
    context->EFlags=(context->EFlags&~0x414u)|desired_flags;
    InterlockedIncrement(&faults);
    return EXCEPTION_CONTINUE_EXECUTION;
}

int main(void)
{
    unsigned int count=0;
    void *registration;
    handler_owner=GetCurrentThreadId();
    registration=AddVectoredExceptionHandler(1,handler);
    if (!registration) return 1;
    for (unsigned int domain=0;domain<2;++domain)
    {
        for (unsigned int combination=0;combination<8;++combination)
        {
            void *request=domain ? NULL : (void *)(uintptr_t)0x2a000000;
            uint64_t *page=VirtualAlloc(request,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
            DWORD old;
            uint64_t actual;
            if (!page || (domain ? (uintptr_t)page<=UINT32_MAX : (void *)page!=request))
                return 1;
            *page=10;
            fault_page=page;
            desired_flags=((combination&1u)<<2)|((combination&2u)<<3)|((combination&4u)<<8);
            InterlockedExchange(&faults,0);
            if (!VirtualProtect(page,4096,PAGE_READONLY,&old)) return 1;
            actual=flag_fault(page);
            if ((actual&0xcd5u)!=desired_flags || *page!=9 || faults!=1)
            {
                fprintf(stderr,"soft-flags-fail domain=%u combination=%u flags=%x expected=%lx faults=%ld\n",
                    domain,combination,(unsigned int)(actual&0xcd5u),(unsigned long)desired_flags,faults);
                return 1;
            }
            fault_page=NULL;
            if (!VirtualFree(page,0,MEM_RELEASE)) return 1;
            ++count;
        }
    }
    if (!RemoveVectoredExceptionHandler(registration) || count!=16) return 1;
    puts("ARM64EC_SOFT_FLAGS_PASS PF AF DF low high edits=16 exact-write-faults=16");
    return 0;
}
