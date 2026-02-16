@echo off
REM NeuStack Windows Setup - delegates to PowerShell script
REM Usage: setup.bat [--no-ai]
powershell -ExecutionPolicy Bypass -File "%~dp0scripts\windows\setup.ps1" %*
