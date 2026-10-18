@echo off
rem Opens the MediEvil launcher on Python 3.14 with the lab switches.
rem The repo's "MediEvil Launcher.cmd" runs "pyw -3", which picks Python 3.8 on this machine
rem (too old for the launcher). GOW3_UE4_BOOT_4K=1: 4K native starts at 4K (menu fix, ebf85f4);
rem the launcher passes its environment to the game.
cd /d "%~dp0..\.."
set GOW3_UE4_BOOT_4K=1
start "" pyw -3.14 launcher\medievil_launcher.py
