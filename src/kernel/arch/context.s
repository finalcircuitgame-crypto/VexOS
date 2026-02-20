.global context_switch

// void context_switch(uint64_t* old_rsp, uint64_t new_rsp)
context_switch:
    // Save current registers
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    
    // Save current stack pointer
    mov %rsp, (%rdi)
    
    // Load new stack pointer
    mov %rsi, %rsp
    
    // Restore registers
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    
    ret
