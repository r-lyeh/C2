@rem C² on Windows:  make.bat [release] [clang] [cl flags...]   (the order doesn't matter)
@rem   make.bat               debug build:     /DNDEBUG /Zi (no optimization, symbols in c2.pdb)
@rem   make.bat release       optimized build: /DNDEBUG /Zi /MT /O2
@rem   make.bat clang         the same with clang-cl;  make.bat clang release, or release clang
@rem   make.bat test [SUITE]  make.bat bench [NAME]  make.bat example  make.bat tour  make.bat tidy
@rem (MIR needs NDEBUG either way: one of its internal checks fails on valid code.)
@if "%1"=="test"  tests\test.bat   %2 %3 %4 %5 %6 %7 %8 %9
@if "%1"=="bench" tests\cbench.bat %2 %3 %4 %5 %6 %7 %8 %9
@if "%1"=="tidy"  rd /q /s .build .build-clang 2> nul
@if "%1"=="tidy"  del *.exe *.obj *.pdb *.ilk 2> nul & exit /b
@if "%1"=="example" call make.bat -DC2_NO_MAIN example.c && example & exit /b
@if "%1"=="tour"  (if not exist c2.exe echo %~dp0c2.exe not found: build it first ^(make^) & exit /b 1)
@if "%1"=="tour"  (if     exist c2.exe for %%i in (tour\*.c2) do @(cls & echo // %%i & echo // --- & type %%i & echo. & c2 %%i & echo. & pause)) & exit /b

@rem Detect platform target (x86, x64 or arm64). We'll use the Developer Prompt's if we're in one, else this machine's
@set ARCH=%VSCMD_ARG_TGT_ARCH%
@if "%ARCH%"=="" set ARCH=%PROCESSOR_ARCHITECTURE%
@if defined PROCESSOR_ARCHITEW6432 if "%VSCMD_ARG_TGT_ARCH%"=="" set ARCH=%PROCESSOR_ARCHITEW6432%
@if /i "%ARCH%"=="AMD64" set ARCH=x64
@if /i "%ARCH%"=="ARM64" set ARCH=arm64
@rem Find Visual Studio (via vswhere, which is shipped with it) and set it up for that target
@where cl >nul 2>nul || (
  for /f "usebackq delims=" %%v in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -prerelease -property installationPath`) do @call "%%v\VC\Auxiliary\Build\vcvarsall.bat" %ARCH% >nul
  @where cl >nul 2>nul || (echo no Visual Studio C++ tools found & pause & exit /b 1)
)

@setlocal
@set "ARGS= %* "
@set "CC=cl /nologo /MP /FS"
@set "FLAGS=/DNDEBUG"
@if not "%ARGS: release =%"=="%ARGS%" set "FLAGS=/DNDEBUG /MT /O2" & set "ARGS=%ARGS: release = %"
@if not "%ARGS: clang =%"=="%ARGS%" set "CC=clang-cl /nologo /D_CRT_SECURE_NO_WARNINGS" & set "ARGS=%ARGS: clang = %"
%CC% %FLAGS% /Zi %ARGS% c2.c 3rd\mir.c 3rd\mir-gen.c /link /STACK:8388608
