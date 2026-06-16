@echo off
REM ---------------------------------------------------------------------------
REM build-gui.bat — configure + build the Zima Qt GUI (gui-qt) and stage it
REM to run. Uses the installed Qt 6 mingw_64 kit + its bundled CMake/Ninja/GCC.
REM ---------------------------------------------------------------------------
setlocal

set "QT=C:\Qt\6.11.1\mingw_64"
set "MINGW=C:\Qt\Tools\mingw1310_64\bin"
set "CMAKE=C:\Qt\Tools\CMake_64\bin\cmake.exe"
set "NINJA=C:\Qt\Tools\Ninja"
set "SRC=%~dp0gui-qt"
set "BUILD=%~dp0build-qt"

set "PATH=%MINGW%;%NINJA%;%PATH%"

REM Configure once (re-run automatically if the build dir is missing).
if not exist "%BUILD%\build.ninja" (
  echo === Configuring ===
  "%CMAKE%" -S "%SRC%" -B "%BUILD%" -G Ninja ^
    -DCMAKE_BUILD_TYPE=Debug ^
    -DCMAKE_PREFIX_PATH="%QT%" ^
    -DCMAKE_CXX_COMPILER="%MINGW%/g++.exe"
  if errorlevel 1 ( echo Configure failed & exit /b 1 )
)

echo === Building ===
"%CMAKE%" --build "%BUILD%"
if errorlevel 1 ( echo Build failed & exit /b 1 )

REM Stage the Qt runtime + plugins and the MinGW runtime DLLs next to the exe.
echo === Deploying runtime ===
"%QT%\bin\windeployqt.exe" --no-translations "%BUILD%\zima-gui-qt.exe" >nul
for %%D in (libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll) do (
  if not exist "%BUILD%\%%D" copy /Y "%MINGW%\%%D" "%BUILD%\" >nul
)

echo.
echo Done:  "%BUILD%\zima-gui-qt.exe"
endlocal
