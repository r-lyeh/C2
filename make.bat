@if "%1"=="test"  tests\test.bat   %2 %3 %4 %5 %6 %7 %8 %9
@if "%1"=="bench" tests\cbench.bat %2 %3 %4 %5 %6 %7 %8 %9
@if "%1"=="tidy"  rd /q /s .build .build-clang 2> nul
@if "%1"=="tidy"  del *.exe *.obj *.pdb *.ilk 2> nul & exit /b
@if "%1"=="clang" clang-cl /nologo /O2 /DNDEBUG /MT /D_CRT_SECURE_NO_WARNINGS c2.c mir\mir.c mir\mir-gen.c & exit /b
@if "%1"=="demo"  call make.bat -DC2_NO_MAIN demo_embed.c && demo_embed & exit /b
@if "%1"=="tour"  c2 demo_tour.c2 & exit /b

cl /nologo /O2 /DNDEBUG /MT /MP %* c2.c mir\mir.c mir\mir-gen.c