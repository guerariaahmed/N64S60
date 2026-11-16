@echo off
rem ------------------------------------------------------------------
rem  N64S60 command-line build (S60 3rd Edition SDK + GCCE, no Carbide)
rem  Run this from a "Command Prompt" where the SDK's devices -setdefault
rem  is already set to your S60 3rd SDK, and the project is on the SAME
rem  drive letter as the SDK (use SUBST if needed).
rem ------------------------------------------------------------------
setlocal
cd /d %~dp0
call bldmake bldfiles || goto :err
call abld build gcce urel || goto :err
makesis N64S60.pkg N64S60_unsigned.sis || goto :err
echo.
echo Built N64S60_unsigned.sis
echo Sign it with your own certificate, e.g.:
echo    signsis N64S60_unsigned.sis N64S60.sis mycert.cer mykey.key
echo (or use Symbian Signed "Open Signed Online" with your phone IMEI)
goto :eof
:err
echo BUILD FAILED - see messages above
exit /b 1
