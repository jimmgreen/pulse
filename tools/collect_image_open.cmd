@echo off
setlocal
chcp 65001 >nul
title Pulse 图片打开诊断
echo 此工具只读取关联、版本和相关日志，不修改默认应用或注册表。
echo 请先在故障电脑上复现一次，再运行本工具。
echo.
set "PULSE_DIAG_PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if defined PROCESSOR_ARCHITEW6432 set "PULSE_DIAG_PS=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
"%PULSE_DIAG_PS%" -STA -NoProfile -ExecutionPolicy Bypass -File "%~dp0diagnose_image_open.ps1"
if errorlevel 1 (
  echo.
  echo 采集未完成。请将上方错误信息反馈给开发者，不需要更改系统策略或管理员权限。
)
echo.
pause
endlocal
