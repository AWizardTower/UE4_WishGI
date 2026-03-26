@echo off
setlocal
set UE_ROOT=F:\UESource\UnrealEngine
set PROJ=F:\UESource\WishGI\WishGI.uproject
call "%UE_ROOT%\Engine\Build\BatchFiles\Build.bat" WishGIEditor Win64 Development -Project="%PROJ%" -WaitMutex
exit /b %errorlevel%
