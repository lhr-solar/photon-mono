@echo off
REM Loopback grok-analyst sidecar (WP-H). Binds 127.0.0.1:18765 only.
REM photon-backend --analysis-compute is off by default; pass 127.0.0.1:18765 to enable.
cd /d "%~dp0\.."
if exist grok-analyst.exe (
  grok-analyst.exe -bind 127.0.0.1:18765 %*
) else (
  go run ./cmd/grok-analyst -bind 127.0.0.1:18765 %*
)
