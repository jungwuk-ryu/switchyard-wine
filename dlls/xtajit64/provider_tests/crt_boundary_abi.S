/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Untimed Win64 nonvolatile-state oracle. Full unwind metadata is required
 * even though intentional access faults run outside this wrapper.
 */
.text
.p2align 4
.globl boundary_check_abi
.def boundary_check_abi; .scl 2; .type 32; .endef
.seh_proc boundary_check_abi
boundary_check_abi:
    pushq %rbx
    .seh_pushreg %rbx
    pushq %rbp
    .seh_pushreg %rbp
    pushq %rsi
    .seh_pushreg %rsi
    pushq %rdi
    .seh_pushreg %rdi
    pushq %r12
    .seh_pushreg %r12
    pushq %r13
    .seh_pushreg %r13
    pushq %r14
    .seh_pushreg %r14
    pushq %r15
    .seh_pushreg %r15
    subq $216,%rsp
    .seh_stackalloc 216
    .irp reg,6,7,8,9,10,11,12,13,14,15
    movdqa %xmm\reg,32+16*(\reg-6)(%rsp)
    .seh_savexmm %xmm\reg,32+16*(\reg-6)
    .endr
    .seh_endprologue
    stmxcsr 192(%rsp)
    fnstcw 200(%rsp)
    movq %rcx,%r10
    movq %rdx,%rcx
    movq %r8,%rdx
    movq %r9,%r8
    movl $0x13571111,%ebx
    movl $0x13572222,%ebp
    movl $0x13573333,%esi
    movl $0x13574444,%edi
    movl $0x13575555,%r12d
    movl $0x13576666,%r13d
    movl $0x13577777,%r14d
    movl $0x13578888,%r15d
    .irp reg,6,7,8,9,10,11,12,13,14,15
    movl $0x24680000+\reg,%eax
    movd %eax,%xmm\reg
    pshufd $0,%xmm\reg,%xmm\reg
    .endr
    callq *%r10
    movq 320(%rsp),%r11
    movq %rax,(%r11)
    stmxcsr 196(%rsp)
    fnstcw 202(%rsp)
    cmpq $0x13571111,%rbx
    jne .Lbad
    cmpq $0x13572222,%rbp
    jne .Lbad
    cmpq $0x13573333,%rsi
    jne .Lbad
    cmpq $0x13574444,%rdi
    jne .Lbad
    cmpq $0x13575555,%r12
    jne .Lbad
    cmpq $0x13576666,%r13
    jne .Lbad
    cmpq $0x13577777,%r14
    jne .Lbad
    cmpq $0x13578888,%r15
    jne .Lbad
    .irp reg,6,7,8,9,10,11,12,13,14,15
    movl $0x24680000+\reg,%eax
    movd %eax,%xmm1
    pshufd $0,%xmm1,%xmm1
    movdqa %xmm\reg,%xmm0
    pcmpeqd %xmm1,%xmm0
    pmovmskb %xmm0,%eax
    cmpl $0xffff,%eax
    jne .Lbad
    .endr
    movl 192(%rsp),%eax
    xorl 196(%rsp),%eax
    testl $0xffc0,%eax /* MXCSR status flags are volatile; control bits are not. */
    jne .Lbad
    movw 200(%rsp),%ax
    cmpw 202(%rsp),%ax
    jne .Lbad
    xorl %eax,%eax
    jmp .Lrestore
.Lbad:
    movl $1,%eax
.Lrestore:
    ldmxcsr 192(%rsp)
    fldcw 200(%rsp)
    .irp reg,6,7,8,9,10,11,12,13,14,15
    movdqa 32+16*(\reg-6)(%rsp),%xmm\reg
    .endr
    addq $216,%rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rbx
    retq
.seh_endproc

/* Intentionally violates Win64 only when explicitly selected as a negative
 * oracle. boundary_check_abi restores its caller before reporting failure.
 */
.p2align 4
.globl boundary_bad_abi
.def boundary_bad_abi; .scl 2; .type 32; .endef
boundary_bad_abi:
    xorl %ebx,%ebx
    retq
