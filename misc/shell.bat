@echo off

REM 'call' means it returns to this after running the other bat file
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
set path=D:\ComputerEnhance\03.1_8086_decoding_mov\misc;%path%