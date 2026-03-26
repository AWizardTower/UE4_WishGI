@echo off
setlocal
set UE_ROOT=F:\UESource\UnrealEngine
set PROJ=F:\UESource\WishGI\WishGI.uproject
call "%UE_ROOT%\GenerateProjectFiles.bat" -project="%PROJ%" -game -engine
exit /b %errorlevel%
