; Only synthetic pattern bytes; copied DWM bodies are NEVER executed.
.CODE
ALIGN 16
PUBLIC FixturePresent
FixturePresent LABEL BYTE
    DB 048h,089h,05Ch,024h,010h,048h,089h,06Ch,024h,018h,056h,057h,041h,054h,041h,056h
    DB 041h,057h,048h,083h,0ECh,040h,045h,08Bh
    DB (0D9h-24) DUP (0CCh)
    DB 0D5h,000h,000h,000h,044h,08Bh,0CDh,04Ch,08Bh,0C6h,048h,08Bh,0D3h,049h,08Bh,0CEh
PUBLIC FixtureCall
FixtureCall LABEL BYTE
    call NEAR PTR FixtureTarget
    DB 08Bh,0F8h,085h,0C0h,00Fh,088h,0FEh,000h
ALIGN 16
PUBLIC FixtureTarget
FixtureTarget LABEL BYTE
    DB 048h,089h,05Ch,024h,020h,055h,056h,057h,041h,054h,041h,055h,041h,056h,041h,057h
    DB 048h,08Dh,06Ch,024h,0D9h,048h,081h,0ECh,0F0h,000h,000h,000h,048h,08Bh,005h,099h
    ret
ALIGN 16
PUBLIC FixtureDuplicate
FixtureDuplicate LABEL BYTE
    DB 64 DUP (0CCh)
END
