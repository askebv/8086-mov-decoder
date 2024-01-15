@echo off

REM 'call' means it returns to this after running the other bat file
REM call "E:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
call "E:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\vsdevcmd.bat" -arch=x64
set path=D:\ComputerEnhance\03.1_8086_decoding_mov\misc;%path%