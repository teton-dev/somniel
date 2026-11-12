/* Entry into and out of guest code, plus the svc entry routine.
 * While guest code runs, tpidr_el0 points at the thread's GuestCtx (see
 * patcher.c); the host's own value is parked in ctx->host_tls. */

#include "guest_offsets.h"

    .text

/* void som_enter_guest(GuestCtx *ctx, pc, sp, arg0, arg1) */
    .global som_enter_guest
    .type som_enter_guest, %function
som_enter_guest:
    stp x19, x20, [x0, #(CTX_CALLEE + 0)]
    stp x21, x22, [x0, #(CTX_CALLEE + 16)]
    stp x23, x24, [x0, #(CTX_CALLEE + 32)]
    stp x25, x26, [x0, #(CTX_CALLEE + 48)]
    stp x27, x28, [x0, #(CTX_CALLEE + 64)]
    stp x29, x30, [x0, #(CTX_CALLEE + 80)]
    stp d8,  d9,  [x0, #(CTX_CALLEE + 96)]
    stp d10, d11, [x0, #(CTX_CALLEE + 112)]
    stp d12, d13, [x0, #(CTX_CALLEE + 128)]
    stp d14, d15, [x0, #(CTX_CALLEE + 144)]
    mov x9, sp
    str x9, [x0, #CTX_HOST_SP]
    mrs x9, tpidr_el0
    str x9, [x0, #CTX_HOST_TLS]
    mov x10, x1
    mov x11, x2
    mov x12, x3
    mov x13, x4
    msr tpidr_el0, x0
    mov sp, x11
    mov x0, x12
    mov x1, x13
    .irp r, 2,3,4,5,6,7,8,9,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
    mov x\r, xzr
    .endr
    br x10
    .size som_enter_guest, . - som_enter_guest

/* void som_leave_guest(GuestCtx *ctx): returns from som_enter_guest */
    .global som_leave_guest
    .type som_leave_guest, %function
som_leave_guest:
    ldr x9, [x0, #CTX_HOST_TLS]
    msr tpidr_el0, x9
    ldp x19, x20, [x0, #(CTX_CALLEE + 0)]
    ldp x21, x22, [x0, #(CTX_CALLEE + 16)]
    ldp x23, x24, [x0, #(CTX_CALLEE + 32)]
    ldp x25, x26, [x0, #(CTX_CALLEE + 48)]
    ldp x27, x28, [x0, #(CTX_CALLEE + 64)]
    ldp x29, x30, [x0, #(CTX_CALLEE + 80)]
    ldp d8,  d9,  [x0, #(CTX_CALLEE + 96)]
    ldp d10, d11, [x0, #(CTX_CALLEE + 112)]
    ldp d12, d13, [x0, #(CTX_CALLEE + 128)]
    ldp d14, d15, [x0, #(CTX_CALLEE + 144)]
    ldr x9, [x0, #CTX_HOST_SP]
    mov sp, x9
    ret
    .size som_leave_guest, . - som_leave_guest

/* uint64_t som_call_guest(ctx x0, fn x1, guest sp x2, args x3 (8 words), return stub x4)
 * Runs a guest function from a host handler. The host frame holds the callee-saved registers, the previous host_sp
 * and call_sp and ctx. While the guest runs, ctx->host_sp is this frame, so kernel calls it makes use the host stack
 * below it. The function returns to the stub, which lands in som_call_return. */
    .global som_call_guest
    .type som_call_guest, %function
som_call_guest:
    sub sp, sp, #0xB0
    stp x29, x30, [sp, #0x00]
    stp x19, x20, [sp, #0x10]
    stp x21, x22, [sp, #0x20]
    stp x23, x24, [sp, #0x30]
    stp x25, x26, [sp, #0x40]
    stp x27, x28, [sp, #0x50]
    ldr x9,  [x0, #CTX_HOST_SP]
    ldr x10, [x0, #CTX_CALL_SP]
    stp x9, x10, [sp, #0x60]
    str x0, [sp, #0x70]
    mov x9, sp
    str x9, [x0, #CTX_HOST_SP]
    str x9, [x0, #CTX_CALL_SP]
    msr tpidr_el0, x0
    mov x30, x4
    mov x10, x1
    mov x11, x2
    mov x12, x3
    ldp x4, x5, [x12, #32]
    ldp x6, x7, [x12, #48]
    ldp x2, x3, [x12, #16]
    ldp x0, x1, [x12, #0]
    mov sp, x11
    br x10
    .size som_call_guest, . - som_call_guest

/* void som_call_return(ctx): leaves the nested guest call, returning its x0 to the caller of som_call_guest */
    .global som_call_return
    .type som_call_return, %function
som_call_return:
    ldr x9, [x0, #CTX_HOST_TLS]
    msr tpidr_el0, x9
    ldr x9, [x0, #CTX_CALL_SP]
    mov sp, x9
    ldr x10, [sp, #0x70]
    ldp x11, x12, [sp, #0x60]
    str x11, [x10, #CTX_HOST_SP]
    str x12, [x10, #CTX_CALL_SP]
    ldr x0, [x10, #(CTX_X + 0)]
    ldp x29, x30, [sp, #0x00]
    ldp x19, x20, [sp, #0x10]
    ldp x21, x22, [sp, #0x20]
    ldp x23, x24, [sp, #0x30]
    ldp x25, x26, [sp, #0x40]
    ldp x27, x28, [sp, #0x50]
    add sp, sp, #0xB0
    ret
    .size som_call_return, . - som_call_return
/* Svc entry. Reached with BL from a per-site stub that already pushed the
 * guest's x29/x30 and put the svc number in x29. This routine is copied into
 * each stub island, so it must stay position independent (no adrp/adr/bl to
 * anything outside it). */
    .global som_svc_common_start
    .global som_svc_common_end
    .balign 16
som_svc_common_start:
    stp x27, x28, [sp, #-16]!
    mrs x28, tpidr_el0                  /* GuestCtx */
    stp x0,  x1,  [x28, #(CTX_X + 0x00)]
    stp x2,  x3,  [x28, #(CTX_X + 0x10)]
    stp x4,  x5,  [x28, #(CTX_X + 0x20)]
    stp x6,  x7,  [x28, #(CTX_X + 0x30)]
    stp x8,  x9,  [x28, #(CTX_X + 0x40)]
    stp x10, x11, [x28, #(CTX_X + 0x50)]
    stp x12, x13, [x28, #(CTX_X + 0x60)]
    stp x14, x15, [x28, #(CTX_X + 0x70)]
    stp x16, x17, [x28, #(CTX_X + 0x80)]
    stp x18, x19, [x28, #(CTX_X + 0x90)]
    stp x20, x21, [x28, #(CTX_X + 0xA0)]
    stp x22, x23, [x28, #(CTX_X + 0xB0)]
    stp x24, x25, [x28, #(CTX_X + 0xC0)]
    str x26,      [x28, #(CTX_X + 0xD0)]
    ldp x0, x1, [sp], #16               /* guest x27, x28 */
    stp x0, x1, [x28, #(CTX_X + 0xD8)]
    ldp x0, x1, [sp]                    /* guest x29, x30 (pushed by the stub) */
    stp x0, x1, [x28, #(CTX_X + 0xE8)]
    add x0, sp, #16
    str x0, [x28, #CTX_SP]              /* guest sp before the stub pushed */
    mrs x0, nzcv
    str x0, [x28, #CTX_NZCV]
    str x30, [x28, #CTX_RET]            /* return address inside the stub */
    str x29, [x28, #CTX_SVC]            /* svc number */
    add x0, x28, #CTX_Q
    stp q0,  q1,  [x0], #32
    stp q2,  q3,  [x0], #32
    stp q4,  q5,  [x0], #32
    stp q6,  q7,  [x0], #32
    stp q8,  q9,  [x0], #32
    stp q10, q11, [x0], #32
    stp q12, q13, [x0], #32
    stp q14, q15, [x0], #32
    stp q16, q17, [x0], #32
    stp q18, q19, [x0], #32
    stp q20, q21, [x0], #32
    stp q22, q23, [x0], #32
    stp q24, q25, [x0], #32
    stp q26, q27, [x0], #32
    stp q28, q29, [x0], #32
    stp q30, q31, [x0], #32
    /* switch to the host: its TLS and its stack */
    ldr x0, [x28, #CTX_HOST_TLS]
    msr tpidr_el0, x0
    ldr x0, [x28, #CTX_HOST_SP]
    mov sp, x0
    mov x19, x28                        /* callee-saved across the call */
    ldr x9, [x28, #CTX_DISPATCH]
    mov x0, x28
    blr x9
    /* back to the guest */
    mov x28, x19
    msr tpidr_el0, x28
    add x0, x28, #CTX_Q
    ldp q0,  q1,  [x0], #32
    ldp q2,  q3,  [x0], #32
    ldp q4,  q5,  [x0], #32
    ldp q6,  q7,  [x0], #32
    ldp q8,  q9,  [x0], #32
    ldp q10, q11, [x0], #32
    ldp q12, q13, [x0], #32
    ldp q14, q15, [x0], #32
    ldp q16, q17, [x0], #32
    ldp q18, q19, [x0], #32
    ldp q20, q21, [x0], #32
    ldp q22, q23, [x0], #32
    ldp q24, q25, [x0], #32
    ldp q26, q27, [x0], #32
    ldp q28, q29, [x0], #32
    ldp q30, q31, [x0], #32
    ldr x0, [x28, #CTX_NZCV]
    msr nzcv, x0
    ldr x0, [x28, #CTX_SP]
    sub sp, x0, #16                     /* where the stub's push sits */
    ldr x30, [x28, #CTX_RET]
    ldp x0,  x1,  [x28, #(CTX_X + 0x00)]
    ldp x2,  x3,  [x28, #(CTX_X + 0x10)]
    ldp x4,  x5,  [x28, #(CTX_X + 0x20)]
    ldp x6,  x7,  [x28, #(CTX_X + 0x30)]
    ldp x8,  x9,  [x28, #(CTX_X + 0x40)]
    ldp x10, x11, [x28, #(CTX_X + 0x50)]
    ldp x12, x13, [x28, #(CTX_X + 0x60)]
    ldp x14, x15, [x28, #(CTX_X + 0x70)]
    ldp x16, x17, [x28, #(CTX_X + 0x80)]
    ldp x18, x19, [x28, #(CTX_X + 0x90)]
    ldp x20, x21, [x28, #(CTX_X + 0xA0)]
    ldp x22, x23, [x28, #(CTX_X + 0xB0)]
    ldp x24, x25, [x28, #(CTX_X + 0xC0)]
    ldr x26,      [x28, #(CTX_X + 0xD0)]
    ldr x27,      [x28, #(CTX_X + 0xD8)]
    ldr x28,      [x28, #(CTX_X + 0xE0)]
    ret
som_svc_common_end:

/* Signal entry shims. If the signal hit guest code, tpidr_el0 still points at a
 * GuestCtx: restore the host's TLS before any C runs, pass in_guest=1 in x3,
 * and put the context pointer back afterwards (sigreturn does not restore
 * tpidr_el0). The target is called as handler(sig, info, uc, in_guest). */
    .macro SIGSHIM name, target
    .global \name
    .type \name, %function
\name:
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    str x19, [sp, #16]
    mov x19, xzr
    mov x3, xzr
    mrs x9, tpidr_el0
    adrp x10, som_ctx_pool_lo
    ldr x10, [x10, :lo12:som_ctx_pool_lo]
    cmp x9, x10
    b.lo 1f
    adrp x11, som_ctx_pool_hi
    ldr x11, [x11, :lo12:som_ctx_pool_hi]
    cmp x9, x11
    b.hs 1f
    mov x19, x9
    mov x3, #1
    ldr x9, [x9, #CTX_HOST_TLS]
    msr tpidr_el0, x9
1:  bl \target
    cbz x19, 2f
    msr tpidr_el0, x19
2:  ldr x19, [sp, #16]
    ldp x29, x30, [sp], #32
    ret
    .size \name, . - \name
    .endm

    SIGSHIM som_crash_shim, som_crash_handler
    SIGSHIM som_suspend_shim, som_suspend_handler

