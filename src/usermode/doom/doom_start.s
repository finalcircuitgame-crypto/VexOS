.section .text.start,"ax"
.global _start
.type _start,@function
_start:
    andq $-16, %rsp
    subq $8, %rsp
    call doom_user_main
    movq $0, %rax
    int $0x80
1:  jmp 1b
.size _start, .-_start
