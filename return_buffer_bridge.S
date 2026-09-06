.text
.p2align 2
.global mod_return_buffer_bridge
.type mod_return_buffer_bridge, %function
mod_return_buffer_bridge:
    .inst 0xd503245f
    sub sp, sp, #224
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x30, [sp, #64]
    str x17, [sp, #80]
    stp q0, q1, [sp, #96]
    stp q2, q3, [sp, #128]
    stp q4, q5, [sp, #160]
    stp q6, q7, [sp, #192]

    mov x0, x8
    bl mod_store_return_buffer

    ldp q6, q7, [sp, #192]
    ldp q4, q5, [sp, #160]
    ldp q2, q3, [sp, #128]
    ldp q0, q1, [sp, #96]
    ldr x17, [sp, #80]
    ldp x8, x30, [sp, #64]
    ldp x6, x7, [sp, #48]
    ldp x4, x5, [sp, #32]
    ldp x2, x3, [sp, #16]
    ldp x0, x1, [sp, #0]
    add sp, sp, #224
    br x17
.size mod_return_buffer_bridge, .-mod_return_buffer_bridge

.section .note.GNU-stack,"",%progbits
