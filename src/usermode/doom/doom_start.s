.section .text.start,"ax"
.global _start
.type _start,@function
_start:
    # The kernel enters with RSP % 16 == 8. Align down so RSP is 16-byte
    # aligned at the CALL boundary; CALL then gives doom_user_main the
    # SysV-required RSP % 16 == 8. Do not subtract another 8 bytes here.
    andq $-16, %rsp
    call doom_user_main
    movq $0, %rax
    int $0x80
1:  jmp 1b
.size _start, .-_start
