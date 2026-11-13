@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=X:\fukami_work\llvm\clang+llvm-23.1.5-x86_64-pc-windows-msvc\bin;%PATH%"
set "D=X:\RECOMPS - THE REAL AFSN\shinobi_port\devilkings-fork"
cd /d "%~dp0"
clang-cl /nologo /std:c++20 /O2 /EHsc /MD /I"%D%\out\build\_deps\raylib-build\raylib\include" /I"%D%\ps2xRuntime\include" fgtest.cpp "%D%\ps2xRuntime\src\lib\ps2_framegen.cpp" /Fe:fgtest.exe /link "%D%\out\build\_deps\raylib-build\raylib\raylib.lib" winmm.lib gdi32.lib opengl32.lib user32.lib shell32.lib
