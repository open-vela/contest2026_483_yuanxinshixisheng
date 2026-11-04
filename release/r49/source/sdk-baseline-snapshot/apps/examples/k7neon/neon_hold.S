/* ARMv8-A only. AAPCS64 preserves LOW 64 bits of v8..v15 across C calls.
 * Upper halves are deliberately neither used nor asserted. No SIMD spill/
 * reload surrounds callback: live values therefore cross actual scheduling.
 * Entry values of d8..d15 are saved solely to honor our caller's ABI. */
.arch armv8-a
.text
.align 2
.global neon_probe_hold
.type neon_probe_hold, %function
neon_probe_hold:
  stp x29, x30, [sp, #-128]!
  mov x29, sp
  stp x19, x20, [sp, #16]
  stp x21, x22, [sp, #32]
  stp d8, d9, [sp, #48]
  stp d10, d11, [sp, #64]
  stp d12, d13, [sp, #80]
  stp d14, d15, [sp, #96]
  mov x19, x1
  mov x20, x2
  mov x21, x3
  mov w22, #0
  ldp d8, d9, [x0]
  ldp d10, d11, [x0, #16]
  ldp d12, d13, [x0, #32]
  ldp d14, d15, [x0, #48]
1:
  mov x0, x21
  blr x20
  cbnz w0, 2f
  fmov v0.2s, #1.0
  fadd v8.2s, v8.2s, v0.2s
  fadd v9.2s, v9.2s, v0.2s
  fadd v10.2s, v10.2s, v0.2s
  fadd v11.2s, v11.2s, v0.2s
  fadd v12.2s, v12.2s, v0.2s
  fadd v13.2s, v13.2s, v0.2s
  fadd v14.2s, v14.2s, v0.2s
  fadd v15.2s, v15.2s, v0.2s
  add w22, w22, #1
  cmp w22, #32
  b.lo 1b
2:
  stp d8, d9, [x19]
  stp d10, d11, [x19, #16]
  stp d12, d13, [x19, #32]
  stp d14, d15, [x19, #48]
  mov w0, w22
  ldp d8, d9, [sp, #48]
  ldp d10, d11, [sp, #64]
  ldp d12, d13, [sp, #80]
  ldp d14, d15, [sp, #96]
  ldp x19, x20, [sp, #16]
  ldp x21, x22, [sp, #32]
  ldp x29, x30, [sp], #128
  ret
.size neon_probe_hold, .-neon_probe_hold
.section .note.GNU-stack,"",%progbits
