EXTERN DispatchHook: PROC

.CODE

; The first 28h bytes are permanent x64 shadow/alignment space. The saved
; HookCpuContext starts at rsp+28h and its returnAddress field aliases the
; return address pushed by dwmcore's original FF 15 call.
AsmLdrpDispatchUserCallTarget PROC FRAME
    sub rsp, 128h
    .allocstack 128h
    .endprolog

    mov [rsp + 028h], rax
    mov [rsp + 030h], rcx
    mov [rsp + 038h], rdx
    mov [rsp + 040h], rbx
    mov [rsp + 048h], rbp
    mov [rsp + 050h], rsi
    mov [rsp + 058h], rdi
    mov [rsp + 060h], r8
    mov [rsp + 068h], r9
    mov [rsp + 070h], r10
    mov [rsp + 078h], r11
    mov [rsp + 080h], r12
    mov [rsp + 088h], r13
    mov [rsp + 090h], r14
    mov [rsp + 098h], r15

    ; Preserve volatile vector arguments used by DWM call targets.
    movdqu [rsp + 0A8h], xmm0
    movdqu [rsp + 0B8h], xmm1
    movdqu [rsp + 0C8h], xmm2
    movdqu [rsp + 0D8h], xmm3
    movdqu [rsp + 0E8h], xmm4
    movdqu [rsp + 0F8h], xmm5

    lea rcx, [rsp + 28h]
    call DispatchHook

    mov rax, [rsp + 028h]
    mov rcx, [rsp + 030h]
    mov rdx, [rsp + 038h]
    mov rbx, [rsp + 040h]
    mov rbp, [rsp + 048h]
    mov rsi, [rsp + 050h]
    mov rdi, [rsp + 058h]
    mov r8,  [rsp + 060h]
    mov r9,  [rsp + 068h]
    mov r10, [rsp + 070h]
    mov r11, [rsp + 078h]
    mov r12, [rsp + 080h]
    mov r13, [rsp + 088h]
    mov r14, [rsp + 090h]
    mov r15, [rsp + 098h]

    movdqu xmm0, [rsp + 0A8h]
    movdqu xmm1, [rsp + 0B8h]
    movdqu xmm2, [rsp + 0C8h]
    movdqu xmm3, [rsp + 0D8h]
    movdqu xmm4, [rsp + 0E8h]
    movdqu xmm5, [rsp + 0F8h]

    add rsp, 128h
    jmp rax
AsmLdrpDispatchUserCallTarget ENDP

END
