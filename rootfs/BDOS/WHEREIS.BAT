@ECHO OFF
REM WHEREIS name - look for a file in the main directories
IF "%1"=="" GOTO usage
FOR %%D IN (C: C:\BDOS C:\GAMES C:\Docs) DO IF EXIST %%D\%1 ECHO %%D\%1
GOTO EOF
:usage
ECHO Usage: WHEREIS filename
