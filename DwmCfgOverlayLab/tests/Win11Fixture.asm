; Synthetic image for scanner/relay regression tests. These bytes come from
; the user-supplied KD disassembly; FixturePresent is NEVER executed.
.CODE
PUBLIC FixturePresent, FixtureCall, FixtureDuplicate

FixtureDispatcher PROC
    jmp rax
FixtureDispatcher ENDP

ALIGN 16
FixturePresent LABEL BYTE
    DB 048h,089h,05Ch,024h,008h,044h,089h,044h,024h,018h,089h,054h,024h,010h
    DB 055h,056h,057h,041h,054h,041h,055h,041h,056h,041h,057h,048h,083h,0ECh,060h
    DB 033h,0DBh,04Ch,08Bh,0E9h,044h,08Bh,0FBh,041h,0F6h,0C0h,002h,00Fh,085h
    DB 0C9h,001h,000h,000h
    DB (086h - ($ - FixturePresent)) DUP (0CCh)
    DB 049h,08Bh,0D4h,048h,08Bh,001h,048h,089h,05Ch,024h,040h,089h,05Ch,024h,038h
    DB 048h,089h,05Ch,024h,030h,048h,08Bh,040h,068h,089h,07Ch,024h,028h
    DB 04Ch,089h,07Ch,024h,020h,044h,08Bh,0BCh,024h,0B0h,000h,000h,000h
    DB 045h,08Bh,0CFh
FixtureCall LABEL BYTE
    call NEAR PTR FixtureThunk
    DB 08Bh,0F8h,085h,0C0h
    DB 080h DUP (0CCh)
FixtureDuplicate LABEL BYTE
    DB 100h DUP (0CCh)

fothk SEGMENT ALIGN(16) 'CODE'
PUBLIC FixtureThunk
FixtureThunk LABEL BYTE
    jmp NEAR PTR FixtureDispatcher
    DB 0Bh DUP (0CCh)
fothk ENDS
END
