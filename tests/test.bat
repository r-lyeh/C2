@echo off
rem Runs the test-suites: ours (c2, c2-errors, c2-abi, c2-reload, c2-debug, c2-tour, c2-book) and andrewchambers_c, lacc, c2m.
rem   test.bat                     everything (needs c2.exe: build it first)
rem   test.bat c2 lacc             only those suites;  -v: why each failure failed;  -k NAME filters
python "%~dp0test.py" %*
