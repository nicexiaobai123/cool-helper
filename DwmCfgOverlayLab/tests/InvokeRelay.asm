.CODE
; Exercise the real relay with four register arguments and five stack
; arguments. No dwmcore function body is executed: RAX is a test target.
InvokeRelay PROC FRAME
    sub rsp, 58h
    .allocstack 58h
    .endprolog
    mov r11, rcx
    mov rax, rdx
    mov ecx, 11h
    mov edx, 22h
    mov r8d, 33h
    mov r9d, 44h
    mov qword ptr [rsp+20h], 55h
    mov qword ptr [rsp+28h], 66h
    mov qword ptr [rsp+30h], 77h
    mov qword ptr [rsp+38h], 88h
    mov qword ptr [rsp+40h], 99h
    call r11
    add rsp, 58h
    ret
InvokeRelay ENDP
END
