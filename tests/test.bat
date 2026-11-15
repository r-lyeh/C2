@echo off
rem Runs every test under tests\ (c2, andrewchambers_c, lacc, c2m; not benchmarks) with both back ends.
rem   test.bat                     everything
rem   test.bat c2 lacc             only those suites (folders of tests\);  -v: why each failure failed;  -k NAME filters
python "%~dp0verify.py" %*
