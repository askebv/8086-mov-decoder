@echo off

IF NOT EXIST ..\build mkdir ..\build
pushd ..\build

REM See compiler argument options:
REM https://learn.microsoft.com/en-us/cpp/build/reference/compiler-options-listed-alphabetically?view=msvc-170
REM Warning code disable reasons:
REM C4201: We never expect to use the /Za compiler (specifically for Windows).
REM C4100: ENABLE IN PRE-RELEASE: Unused parameters will happen all the time in debug mode.
REM C4189: ENABLE IN PRE-RELEASE: Unused variables will happen all the time in debug mode.
REM -W4 -WX
REM -MT
set CommonCompilerFlags=-nologo -Od -wd4201 -wd4100 -wd4189 -wd4505 -wd4127 -Gm- -GR- -EHa- -Oi -FC -Z7 -Fm8086_decoder.map
set CommonCompilerFlags=-DASH_INTERNAL=1 -DASH_SLOW=1 %CommonCompilerFlags%
set CommonLinkerFlags=-opt:ref -incremental:no

REM TODO: Replace -Od with -O2 when not in learning mode
REM If debugging is weird, consider -Zi instead of Z7
REM -subsystem:windows,6.1 means Windows 7 is the minimum requirement
REM -MT integrates the OS C++RT link, instead of searching for an DLL
REM -Fm8086_decoder.map which asks the linker to show a map of functions for the executable
REM TODO: Test with Dependency Walker

REM 64bit build
REM del *.pdb > NUL 2> NUL
cl %CommonCompilerFlags% "..\code\8086_decoder.cpp" /link %CommonLinkerFlags%

popd