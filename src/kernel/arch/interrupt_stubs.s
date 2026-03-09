.extern irq0_handler
.extern syscall_handler
.extern exception_handler
.global irq0_stub
.global syscall_stub
.global isr0_stub
.global isr1_stub
.global isr2_stub
.global isr3_stub
.global isr4_stub
.global isr5_stub
.global isr6_stub
.global isr7_stub
.global isr8_stub
.global isr9_stub
.global isr10_stub
.global isr11_stub
.global isr12_stub
.global isr13_stub
.global isr14_stub
.global isr15_stub
.global isr16_stub
.global isr17_stub
.global isr18_stub
.global isr19_stub
.global isr20_stub
.global isr21_stub
.global isr22_stub
.global isr23_stub
.global isr24_stub
.global isr25_stub
.global isr26_stub
.global isr27_stub
.global isr28_stub
.global isr29_stub
.global isr30_stub
.global isr31_stub

.macro PUSH_GPRS
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
.endm

.macro POP_GPRS
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax
.endm

.macro ISR_NOERR vec
.global isr\vec\()_stub
isr\vec\()_stub:
    pushq $0
    pushq $\vec
    PUSH_GPRS
    mov %rsp, %rdi
    call exception_handler
    cli
    hlt
.endm

.macro ISR_ERR vec
.global isr\vec\()_stub
isr\vec\()_stub:
    pushq $\vec
    PUSH_GPRS
    mov %rsp, %rdi
    call exception_handler
    cli
    hlt
.endm

irq0_stub:
    /* Save all registers */
    PUSH_GPRS

    mov %rsp, %rdi /* Pass stack pointer as argument */
    call irq0_handler

    /* If irq0_handler returns a new RSP, switch to it */
    mov %rax, %rsp

    POP_GPRS
    
    iretq

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

syscall_stub:
    /* Save all registers */
    PUSH_GPRS

    mov %rsp, %rdi /* Pass stack pointer as argument */
    call syscall_handler

    /* syscall_handler returns (possibly updated) RSP */
    mov %rax, %rsp

    POP_GPRS

    iretq