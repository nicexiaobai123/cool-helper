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

; Windows 11 fothk call sites use E8 to reach a nearby relay. The relay calls
; this bridge and then jumps back to the original fothk thunk, preserving the
; platform's XFG/CFG dispatch. Unlike the legacy bridge this procedure returns
; to the relay instead of tail-jumping to the virtual target in RAX.
AsmFothkCallSiteBridge PROC FRAME
    ; The relay CALL adds one more return address than the legacy path. A
    ; 120h frame keeps RSP 16-byte aligned before DispatchHook; the context
    ; starts after the 20h shadow area and still aliases its returnAddress at
    ; offset 100h with the relay return address at rsp+120h.
    sub rsp, 120h
    .allocstack 120h
    .endprolog

    mov [rsp + 020h], rax
    mov [rsp + 028h], rcx
    mov [rsp + 030h], rdx
    mov [rsp + 038h], rbx
    mov [rsp + 040h], rbp
    mov [rsp + 048h], rsi
    mov [rsp + 050h], rdi
    mov [rsp + 058h], r8
    mov [rsp + 060h], r9
    mov [rsp + 068h], r10
    mov [rsp + 070h], r11
    mov [rsp + 078h], r12
    mov [rsp + 080h], r13
    mov [rsp + 088h], r14
    mov [rsp + 090h], r15

    movdqu [rsp + 0A0h], xmm0
    movdqu [rsp + 0B0h], xmm1
    movdqu [rsp + 0C0h], xmm2
    movdqu [rsp + 0D0h], xmm3
    movdqu [rsp + 0E0h], xmm4
    movdqu [rsp + 0F0h], xmm5

    lea rcx, [rsp + 20h]
    call DispatchHook

    mov rax, [rsp + 020h]
    mov rcx, [rsp + 028h]
    mov rdx, [rsp + 030h]
    mov rbx, [rsp + 038h]
    mov rbp, [rsp + 040h]
    mov rsi, [rsp + 048h]
    mov rdi, [rsp + 050h]
    mov r8,  [rsp + 058h]
    mov r9,  [rsp + 060h]
    mov r10, [rsp + 068h]
    mov r11, [rsp + 070h]
    mov r12, [rsp + 078h]
    mov r13, [rsp + 080h]
    mov r14, [rsp + 088h]
    mov r15, [rsp + 090h]

    movdqu xmm0, [rsp + 0A0h]
    movdqu xmm1, [rsp + 0B0h]
    movdqu xmm2, [rsp + 0C0h]
    movdqu xmm3, [rsp + 0D0h]
    movdqu xmm4, [rsp + 0E0h]
    movdqu xmm5, [rsp + 0F0h]

    add rsp, 120h
    ret
AsmFothkCallSiteBridge ENDP

END
