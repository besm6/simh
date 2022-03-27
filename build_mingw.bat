@echo off
:: Compile all of SIMH using MINGW make and gcc environment
::
:: The makefile will determine if the needed WinPcap build 
:: components are available and the resulting simulators will
:: run with networking support when the WinPcap environment 
:: is installed on the running system
::
:: Individual simulator sources are in .\simulator_name
:: Individual simulator executables are to .\BIN
::
:: If needed, define the path for the MINGW bin directory

gcc -v 1>NUL 2>NUL
if ERRORLEVEL 1 path C:\MinGW\bin;%path%
if not exist BIN mkdir BIN
gcc -v 1>NUL 2>NUL
if ERRORLEVEL 1 echo "MinGW Environment Unavailable"
mingw32-make -f makefile %*
