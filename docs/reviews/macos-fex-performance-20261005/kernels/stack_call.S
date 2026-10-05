/* Win64-only measurement adapter, NOT a general SEH/unwind implementation.
 * Outside the kernel bytes. Match its identity guest stack with the SDK harness.
 * Synthetic kernels cannot fault/callback and preserve R12. */
.text
.globl sb_stack_call
.seh_proc sb_stack_call
sb_stack_call:
    push %r12
    .seh_pushreg %r12
    .seh_endprologue
    mov %rsp,%r12
    mov 48(%rsp),%rax
    mov %rcx,%r11
    mov %rdx,%rcx
    mov %r8,%rdx
    mov %r9,%r8
    mov %rax,%rsp
    sub $32,%rsp
    call *%r11
    mov %r12,%rsp
    pop %r12
    ret
.seh_endproc
