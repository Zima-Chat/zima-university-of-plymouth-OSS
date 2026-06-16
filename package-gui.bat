@echo off
REM ---------------------------------------------------------------------------
REM package-gui.bat — produce a distributable build of the Zima Qt GUI.
REM
REM Qt is a shared (DLL) build, so the app can't be a single static exe. Instead
REM this gathers exactly the required Qt DLLs/plugins + MinGW runtime into one
REM self-contained folder (dist\Zima) and wraps it as:
REM   * dist\Zima-win64.zip   — portable, unzip-and-run
REM   * dist\Zima-Setup.exe    — one-file self-extractor (needs 7-Zip)
REM ---------------------------------------------------------------------------
setlocal

set "QT=C:\Qt\6.11.1\mingw_64"
set "MINGW=C:\Qt\Tools\mingw1310_64\bin"
set "CMAKE=C:\Qt\Tools\CMake_64\bin\cmake.exe"
set "NINJA=C:\Qt\Tools\Ninja"
set "SZ=C:\Program Files\7-Zip"
set "ROOT=%~dp0"
set "SRC=%ROOT%gui-qt"
set "BUILD=%ROOT%build-qt-release"
set "DIST=%ROOT%dist"
set "STAGE=%DIST%\Zima"
set "PATH=%MINGW%;%NINJA%;%PATH%"

echo === Configure + build (Release) ===
if not exist "%BUILD%\build.ninja" "%CMAKE%" -S "%SRC%" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%QT%" -DCMAKE_CXX_COMPILER="%MINGW%/g++.exe"
"%CMAKE%" --build "%BUILD%"
if errorlevel 1 ( echo Build failed & exit /b 1 )

echo === Stage self-contained folder ===
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%"
copy /Y "%BUILD%\zima-gui-qt.exe" "%STAGE%\Zima.exe" >nul
"%MINGW%\strip.exe" "%STAGE%\Zima.exe"
"%QT%\bin\windeployqt.exe" --release --no-translations --compiler-runtime "%STAGE%\Zima.exe" >nul
for %%D in (libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll) do if not exist "%STAGE%\%%D" copy /Y "%MINGW%\%%D" "%STAGE%\" >nul

echo === Portable ZIP ===
if exist "%DIST%\Zima-win64.zip" del "%DIST%\Zima-win64.zip"
"%SZ%\7z.exe" a -tzip "%DIST%\Zima-win64.zip" "%STAGE%" >nul

echo === One-file self-extractor ===
if exist "%SZ%\7z.sfx" (
  > "%DIST%\sfx.txt" echo ;!@Install@!UTF-8!
  >> "%DIST%\sfx.txt" echo Title="Zima"
  >> "%DIST%\sfx.txt" echo ;!@InstallEnd@!
  if exist "%DIST%\Zima.7z" del "%DIST%\Zima.7z"
  "%SZ%\7z.exe" a -t7z -mx=9 "%DIST%\Zima.7z" "%STAGE%" >nul
  copy /b "%SZ%\7z.sfx" + "%DIST%\sfx.txt" + "%DIST%\Zima.7z" "%DIST%\Zima-Setup.exe" >nul
  del "%DIST%\Zima.7z" "%DIST%\sfx.txt"
) else (
  echo 7-Zip not found at %SZ% - skipping self-extractor.
)

echo.
echo Done:
echo   Self-contained folder : %STAGE%
echo   Portable ZIP          : %DIST%\Zima-win64.zip
echo   One-file extractor    : %DIST%\Zima-Setup.exe
endlocal
