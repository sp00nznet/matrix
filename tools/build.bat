@echo off
rem Build the host for one target.
rem
rem   tools\build.bat final                 full build  -> build\mxo_final.exe
rem   tools\build.bat final src\runtime\host.c ...   recompile only those, relink
rem   tools\build.bat final trace            full traced build -> build\mxo_final_trace.exe
rem   tools\build.bat retail ...             same, for the retail client (mxo_host.exe)
rem
rem MSVC 2022 x86 (BuildTools). A full build compiles ~180 generated files and
rem takes several minutes; after editing runtime files, pass just those.
rem Expects src\runtime\_imports_active.c and _interfaces_active.c to be the
rem target's generated tables (copies of imports_gen_<t>.c / interfaces_gen_<t>.c).
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul 2>nul
cd /d "%~dp0.."

set T=%1
shift
if "%T%"=="final"  (set GEN=src\recomp\gen_final& set OBJ=build\obj_final& set EXE=build\mxo_final)
if "%T%"=="retail" (set GEN=src\recomp\gen& set OBJ=build\obj& set EXE=build\mxo_host)
if not defined GEN (echo usage: tools\build.bat final^|retail [trace] [files...] & exit /b 2)

set DEF=
if "%1"=="trace" (set DEF=/DRECOMP_TRACE& set OBJ=%OBJ%_trace& set EXE=%EXE%_trace& shift)
if not exist %OBJ% mkdir %OBJ%

set R=src\runtime
set FILES=%1 %2 %3 %4 %5 %6 %7 %8 %9
rem The launcher's lift (src\recomp\gen_launcher, imports_gen_launcher.c) links
rem into both targets; it only runs when MXO_LOGIN asks for a login.
if "%1"=="" set FILES=%GEN%\*.c src\recomp\gen_launcher\*.c %R%\imports_gen_launcher.c %R%\launcher_host.c %R%\_imports_active.c %R%\_interfaces_active.c %R%\engine_iface.c %R%\host.c %R%\image_loader.c %R%\native_call.c %R%\guest_call.c %R%\guest_threads.c %R%\login_mediator.c %R%\recomp_trace.c %R%\shims_crt.c %R%\shims_impl.c %R%\shims_mslu.c %R%\shims_net.c %R%\shims_win32.c

cl /nologo /c /O2 /MP %DEF% /I%R% /I%GEN% /Fo%OBJ%\ %FILES%
if errorlevel 1 exit /b 1
link /nologo /SUBSYSTEM:CONSOLE /LARGEADDRESSAWARE:NO /OUT:%EXE%.exe %OBJ%\*.obj kernel32.lib user32.lib gdi32.lib ws2_32.lib winmm.lib advapi32.lib version.lib ole32.lib rpcrt4.lib
