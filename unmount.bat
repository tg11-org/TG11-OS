@echo off
setlocal

set "VHD=U:\Projects\TG11-OS\TG11-DATA.vhd"
set "DPSCRIPT=%TEMP%\unmount_tg11_data_diskpart.txt"

(
    echo select vdisk file="%VHD%"
    echo detach vdisk
) > "%DPSCRIPT%"

diskpart /s "%DPSCRIPT%"
set "ERR=%ERRORLEVEL%"
del "%DPSCRIPT%" >nul 2>&1

if not "%ERR%"=="0" (
    echo Failed to unmount VHD.
    echo Make sure this .bat is run as Administrator.
    pause
    exit /b %ERR%
)

echo Unmounted:
echo %VHD%
pause

rem Copyright (C) 2026 TG11
rem 
rem This program is free software: you can redistribute it and/or modify
rem it under the terms of the GNU Affero General Public License as
rem published by the Free Software Foundation, either version 3 of the
rem License, or (at your option) any later version.
rem 
rem This program is distributed in the hope that it will be useful,
rem but WITHOUT ANY WARRANTY; without even the implied warranty of
rem MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
rem GNU Affero General Public License for more details.
rem 
rem You should have received a copy of the GNU Affero General Public License
rem along with this program.  If not, see <https://www.gnu.org/licenses/>.