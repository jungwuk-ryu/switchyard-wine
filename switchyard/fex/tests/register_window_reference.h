// SPDX-License-Identifier: LGPL-2.1-or-later
// Independent pre-window Wine conversion contract. Keep this oracle explicit:
// deriving its GPR order/defaults from window offsets would share regressions.
// The production provider checks every field against its real Wine layout.
#pragma once
#include <cstdint>
#include <cstring>
using UINT64 = uint64_t;
using UINT32 = uint32_t;
struct xtajit64_x64_context
{
    UINT64 rax, rbx, rcx, rdx;
    UINT64 rsi, rdi, rbp, rsp;
    UINT64 r8, r9, r10, r11;
    UINT64 r12, r13, r14, r15;
    UINT64 rip, eflags;
    UINT32 mxcsr;
    UINT32 reserved;
    UINT64 xmm[16][2];
};
static void import_fex_state( struct switchyard_fex_x64_state *dst,
                              const struct xtajit64_x64_context *src,
                              uint64_t gs_base )
{
    /* Initialize the complete payload without clearing registers that are
     * immediately overwritten. Keep even the invalid YMM high halves defined
     * for all adapter vector layouts. */
    dst->size = sizeof(*dst);
    dst->version = SWITCHYARD_FEX_STATE_VERSION;
    dst->flags = 0;
    dst->reserved = 0;
    dst->gpr[0] = src->rax;
    dst->gpr[1] = src->rcx;
    dst->gpr[2] = src->rdx;
    dst->gpr[3] = src->rbx;
    dst->gpr[4] = src->rsp;
    dst->gpr[5] = src->rbp;
    dst->gpr[6] = src->rsi;
    dst->gpr[7] = src->rdi;
    dst->gpr[8] = src->r8;
    dst->gpr[9] = src->r9;
    dst->gpr[10] = src->r10;
    dst->gpr[11] = src->r11;
    dst->gpr[12] = src->r12;
    dst->gpr[13] = src->r13;
    dst->gpr[14] = src->r14;
    dst->gpr[15] = src->r15;
    dst->rip = src->rip;
    dst->rflags = src->eflags;
    dst->mxcsr = src->mxcsr;
    dst->fcw = 0x037f;
    dst->abridged_ftw = 0;
    dst->reserved_fp = 0;
    memset( dst->segment, 0, sizeof(dst->segment) );
    dst->segment[1] = 0x30;
    dst->reserved_segment = 0;
    memset( dst->segment_base, 0, sizeof(dst->segment_base) );
    dst->segment_base[5] = gs_base;
    memcpy( dst->xmm, src->xmm, sizeof(src->xmm) );
    memset( dst->ymm_high, 0, sizeof(dst->ymm_high) );
    memset( dst->x87, 0, sizeof(dst->x87) );
}

static void export_fex_state( struct xtajit64_x64_context *dst,
                              const struct switchyard_fex_x64_state *src )
{
    dst->rax = src->gpr[0];
    dst->rcx = src->gpr[1];
    dst->rdx = src->gpr[2];
    dst->rbx = src->gpr[3];
    dst->rsp = src->gpr[4];
    dst->rbp = src->gpr[5];
    dst->rsi = src->gpr[6];
    dst->rdi = src->gpr[7];
    dst->r8 = src->gpr[8];
    dst->r9 = src->gpr[9];
    dst->r10 = src->gpr[10];
    dst->r11 = src->gpr[11];
    dst->r12 = src->gpr[12];
    dst->r13 = src->gpr[13];
    dst->r14 = src->gpr[14];
    dst->r15 = src->gpr[15];
    dst->rip = src->rip;
    dst->eflags = src->rflags;
    dst->mxcsr = src->mxcsr;
    dst->reserved = 0;
    memcpy( dst->xmm, src->xmm, sizeof(dst->xmm) );
}
