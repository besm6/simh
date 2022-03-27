@echo off
:: Build without SIM_ASYNCH_IO defined (avoiding the use of pthreads)
:: Compile all of SIMH using MINGW make and gcc environment
:: Individual simulator sources are in .\simulator_name
:: Individual simulator executables are to .\BIN
::
:: If needed, define the path for the MINGW bin directory

gcc -v 1>NUL 2>NUL
if ERRORLEVEL 1 path C:\MinGW\bin;%path%
if not exist BIN mkdir BIN
gcc -v 1>NUL 2>NUL
if ERRORLEVEL 1 echo "MinGW Environment Unavailable"
mingw32-make NOASYNCH=1 -f makefile %*
