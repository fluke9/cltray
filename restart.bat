@echo off
taskkill /IM cltray.exe /F 2>nul
start "" cltray.exe
