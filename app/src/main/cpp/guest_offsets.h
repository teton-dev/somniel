#pragma once
/* Byte offsets into GuestCtx, shared by C (checked with static_assert in
 * guest.c) and the assembly in guest_asm.S. Change them in both places. */

#define CTX_X          0x000   /* x0..x30 at 8-byte steps */
#define CTX_SP         0x0F8
#define CTX_NZCV       0x100
#define CTX_RET        0x108
#define CTX_HOST_SP    0x110
#define CTX_HOST_TLS   0x118
#define CTX_GUEST_TPIDR 0x120
#define CTX_TLR        0x128
#define CTX_SVC        0x130
#define CTX_DISPATCH   0x138
#define CTX_CALLEE     0x140   /* x19..x30, d8..d15: 20 * 8 bytes */
#define CTX_Q          0x1F0   /* q0..q31 */

#define CTX_CALL_SP    0x3F0   /* host-only: frame of the innermost host->guest call */

