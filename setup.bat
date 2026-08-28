@echo off
rem One-shot bootstrap for a fresh clone:
rem   1. initialize submodules (krkrz etc.)
rem   2. download + verify + extract the game data from the data source
rem      release (the location is recorded in data-source.json)
setlocal
cd /d "%~dp0"

git submodule sync --recursive || goto :err
git submodule update --init --recursive || goto :err
python tools\fetch_data_parts.py --dest data || goto :err

echo setup complete: submodules ready, game data in data\
exit /b 0

:err
echo setup FAILED
exit /b 1
