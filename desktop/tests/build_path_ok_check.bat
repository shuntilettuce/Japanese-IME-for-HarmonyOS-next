@echo off
rem Build path_ok_check.exe (QWERTY romaji typo correction measured through the Composer). Usage is at the top of the .cpp
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set ROOT=%~dp0..\..
set OUT=%~dp0..\build\test
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OUT%\engine_avx2.obj" cl /nologo /c /O2 /EHsc /std:c++17 /utf-8 /W3 /arch:AVX2 /Fo"%OUT%\engine_avx2.obj" "%ROOT%\entry\src\main\cpp\engine_avx2.cpp" || exit /b 1
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /W3 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I"%ROOT%\entry\src\main\cpp" /Fo"%OUT%\\" /Fe"%OUT%\path_ok_check.exe" ^
  "%~dp0path_ok_check.cpp" "%ROOT%\desktop\core\text.cpp" "%ROOT%\desktop\core\romaji.cpp" "%ROOT%\desktop\core\store.cpp" ^
  "%ROOT%\desktop\core\converter.cpp" "%ROOT%\desktop\core\style.cpp" "%ROOT%\desktop\core\release_info.cpp" "%ROOT%\desktop\core\composer.cpp" ^
  "%ROOT%\desktop\core\flow.cpp" "%ROOT%\desktop\core\layout.cpp" "%ROOT%\desktop\core\typo_alts.cpp" "%ROOT%\desktop\core\datetime.cpp" ^
  "%ROOT%\desktop\core\settings.cpp" "%ROOT%\entry\src\main\cpp\engine.cpp" "%OUT%\engine_avx2.obj" || exit /b 1
