# GAS (AT&T) port of S-inject's S-Wisper-asm.x64.asm, for MinGW/x64.
# Sw3NtCreateThreadEx resolves the NtCreateThreadEx SSN at runtime and calls
# it directly via `syscall`, bypassing CreateRemoteThread wire-up detection.
.text
.global Sw3NtCreateThreadEx
.def	Sw3NtCreateThreadEx
.scl	2
.type	32
.endef
.extern SW3_GetSyscallNumber

Sw3NtCreateThreadEx:
    movq %rcx,  8(%rsp)       # save registers
    movq %rdx, 16(%rsp)
    movq %r8,  24(%rsp)
    movq %r9,  32(%rsp)
    subq $0x28, %rsp
    movl $0x84A94AFE, %ecx    # function hash
    call SW3_GetSyscallNumber # resolve hash -> SSN (returned in eax)
    addq $0x28, %rsp
    movq  8(%rsp), %rcx       # restore registers
    movq 16(%rsp), %rdx
    movq 24(%rsp), %r8
    movq 32(%rsp), %r9
    movq %rcx, %r10           # arg1 lives in r10 for syscalls
    syscall
    ret