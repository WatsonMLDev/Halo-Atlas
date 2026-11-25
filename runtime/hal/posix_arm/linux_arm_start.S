.syntax unified
.thumb

.text

.global _start
.type _start, %function
_start:
    ldr r0, [sp]
    add r1, sp, #4
    mov r2, sp
    bic r2, r2, #7
    mov sp, r2
    bl runner_main
    mov r7, #248          /* __NR_exit_group */
    svc #0
1:
    b 1b

/*
 * int32_t linux_syscall6(uint32_t nr, uint32_t a0, uint32_t a1, uint32_t a2,
 *                        uint32_t a3, uint32_t a4, uint32_t a5)
 */
.global linux_syscall6
.type linux_syscall6, %function
linux_syscall6:
    push {r4, r5, r7, lr}
    mov r7, r0
    mov r0, r1
    mov r1, r2
    mov r2, r3
    ldr r3, [sp, #16]
    ldr r4, [sp, #20]
    ldr r5, [sp, #24]
    svc #0
    pop {r4, r5, r7, pc}
