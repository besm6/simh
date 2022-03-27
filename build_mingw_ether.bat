@echo off

:: Built in Ethernet support (requires WinPcap installed).
:: The normal Windows build always builds with Ethernet support
:: so, this procedure is un-necessary. Just call the normal build

%~p0\build_mingw.bat %*
