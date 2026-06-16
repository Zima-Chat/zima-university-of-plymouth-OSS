@echo off
REM ---------------------------------------------------------------------------
REM clean.bat — remove all build / packaging artifacts from the source tree.
REM Leaves source and the vendored dependencies (vendor\libsodium, etc.) intact.
REM ---------------------------------------------------------------------------
setlocal
set "ROOT=%~dp0"

echo Cleaning build artifacts...

REM Build output directories (Qt CMake, MinGW make, MSVC).
for %%D in (build-qt build-qt-release dist build bin obj .vs) do (
  if exist "%ROOT%%%D" (
    echo   removing %%D\
    rmdir /s /q "%ROOT%%%D"
  )
)

REM Stray logs / intermediates.
if exist "%ROOT%vendor\_build" (
  echo   removing vendor\_build\
  rmdir /s /q "%ROOT%vendor\_build"
)
if exist "%ROOT%vendor\_deps-build.log" del /q "%ROOT%vendor\_deps-build.log"

echo Done. Source and vendored deps are untouched.
endlocal
