@echo off
taskkill /IM cltray.exe /F 2>nul
REM Build with MSVC (run from a Developer Command Prompt)
rc /nologo /fo resource.res resource.rc
cl /O2 /W4 /DUNICODE /D_UNICODE main.c parson.c /link /SUBSYSTEM:WINDOWS shell32.lib user32.lib gdi32.lib advapi32.lib winhttp.lib resource.res /OUT:cltray.exe

REM Alternatively, build with MinGW:
REM gcc -O2 -mwindows -DUNICODE -D_UNICODE main.c -o cltray.exe -lshell32 -lgdi32
