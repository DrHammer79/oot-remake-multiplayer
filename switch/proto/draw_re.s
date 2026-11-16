// Research taps for the draw-re build (COOP_DRAW_RE, switch/proto/draw_re.h): log only, change nothing.
//
// Each tap replaces one `bl` of the game (hook table kCallRedirect): it saves the argument registers, x8, the
// callee-saved x19..x22 the caller keeps its objects in, and q0..q7; calls coop_draw_re_tap(which, regs) with
// regs = {x0..x7, x29, x30, x8, x19, x20, x21, x22}; restores everything; then jumps through its slot to the
// game function the `bl` called, as if the game had called it (same shape as coop_frame_entry in start.s).

    .macro draw_re_tap name, which, slot
    .section .text, "ax"
    .global \name
    .protected \name
    .type \name, %function
\name:
    .cfi_startproc
    sub     sp, sp, #0x100
    .cfi_def_cfa_offset 0x100
    stp     x0, x1, [sp, #0x00]
    stp     x2, x3, [sp, #0x10]
    stp     x4, x5, [sp, #0x20]
    stp     x6, x7, [sp, #0x30]
    stp     x29, x30, [sp, #0x40]
    stp     x8, x19, [sp, #0x50]
    stp     x20, x21, [sp, #0x60]
    str     x22, [sp, #0x70]
    stp     q0, q1, [sp, #0x80]
    stp     q2, q3, [sp, #0xa0]
    stp     q4, q5, [sp, #0xc0]
    stp     q6, q7, [sp, #0xe0]
    .cfi_offset x29, -0xc0
    .cfi_offset x30, -0xb8
    add     x29, sp, #0x40
    mov     w0, #\which
    mov     x1, sp
    bl      coop_draw_re_tap
    ldp     q6, q7, [sp, #0xe0]
    ldp     q4, q5, [sp, #0xc0]
    ldp     q2, q3, [sp, #0xa0]
    ldp     q0, q1, [sp, #0x80]
    ldr     x22, [sp, #0x70]
    ldp     x20, x21, [sp, #0x60]
    ldp     x8, x19, [sp, #0x50]
    ldp     x29, x30, [sp, #0x40]
    ldp     x6, x7, [sp, #0x30]
    ldp     x4, x5, [sp, #0x20]
    ldp     x2, x3, [sp, #0x10]
    ldp     x0, x1, [sp, #0x00]
    add     sp, sp, #0x100
    .cfi_def_cfa_offset 0
    .cfi_restore x29
    .cfi_restore x30
    adrp    x16, \slot
    ldr     x16, [x16, :lo12:\slot]
    br      x16
    .cfi_endproc
    .size \name, . - \name
    .endm

    draw_re_tap coop_draw_re_apply_entry, 0, coop_slot_draw_re_apply
    draw_re_tap coop_draw_re_apply_default_entry, 1, coop_slot_draw_re_apply_default
    draw_re_tap coop_draw_re_register_entry, 2, coop_slot_draw_re_register
