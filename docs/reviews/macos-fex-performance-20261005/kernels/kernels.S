/* Synthetic Win64 leaf kernels. One assembled payload, no relocations.
 * RCX=count,RDX=data,R8=seed. RAX=result. Preserve every nonvolatile.
 * Data:256 immutable uint64_t inputs followed by256 uint64_t outputs.
 * Both embeddings copy THESE bytes to the same high identity address. */
.text
.p2align 8
.globl sb_register
sb_register:
    mov %r8,%rax
    test %rcx,%rcx
    jz .Lreg_done
.Lreg_loop:
    add %r8,%rax
    add $3,%rax
    dec %rcx
    jnz .Lreg_loop
.Lreg_done:
    ret

.p2align 8
.globl sb_scalar
sb_scalar:
    mov %r8,%rax
    xor %r9d,%r9d
    test %rcx,%rcx
    jz .Lscalar_done
.Lscalar_loop:
    add (%rdx,%r9,8),%rax
    mov %rax,2048(%rdx,%r9,8)
    inc %r9
    and $255,%r9d
    dec %rcx
    jnz .Lscalar_loop
.Lscalar_done:
    ret

.p2align 8
.globl sb_simd
sb_simd:
    movq %r8,%xmm0
    xor %r9d,%r9d
    test %rcx,%rcx
    jz .Lsimd_done
.Lsimd_loop:
    movdqu (%rdx,%r9),%xmm1
    paddq %xmm1,%xmm0
    movdqu %xmm0,2048(%rdx,%r9)
    add $16,%r9d
    and $2047,%r9d
    dec %rcx
    jnz .Lsimd_loop
.Lsimd_done:
    movq %xmm0,%rax
    ret

.p2align 8
.globl sb_direct
sb_direct:
    mov %r8,%rax
    test %rcx,%rcx
    jz .Ldirect_done
.Ldirect_loop:
    call .Ldirect_callee
    dec %rcx
    jnz .Ldirect_loop
.Ldirect_done:
    ret
.Ldirect_callee:
    add %r8,%rax
    add $3,%rax
    ret

.p2align 8
.globl sb_indirect
sb_indirect:
    mov %r8,%rax
    lea .Lindirect_callee(%rip),%r10
    test %rcx,%rcx
    jz .Lindirect_done
.Lindirect_loop:
    call *%r10
    dec %rcx
    jnz .Lindirect_loop
.Lindirect_done:
    ret
.Lindirect_callee:
    add %r8,%rax
    add $3,%rax
    ret

/* Control: same arithmetic and one real stack push/pop per iteration,
 * without an inner call, return, or shadow predictor update. The pushed
 * value is this exact pop-label address, independently checked in C. */
.p2align 8
.globl sb_stack
sb_stack:
    mov %r8,%rax
    lea sb_stack_pop(%rip),%r10
    test %rcx,%rcx
    jz .Lstack_done
.Lstack_loop:
    push %r10
    add %r8,%rax
    add $3,%rax
.globl sb_stack_pop
sb_stack_pop:
    pop %r10
    dec %rcx
    jnz .Lstack_loop
.Lstack_done:
    ret
