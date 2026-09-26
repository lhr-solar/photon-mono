@echo off
setlocal
cd /d "%~dp0"
rem Loopback only. Does not open :6100/:6500/:6700. No TSDB.
rem Per-job containers: grok-analyst-job:local (no docker.sock on workers).
if "%GROK_ANALYST_BIND%"=="" set GROK_ANALYST_BIND=127.0.0.1:18765
if "%GROK_ANALYST_JOB_IMAGE%"=="" set GROK_ANALYST_JOB_IMAGE=grok-analyst-job:local
docker build -t %GROK_ANALYST_JOB_IMAGE% -f Dockerfile.job .
if errorlevel 1 (
  echo [grok-analyst] job image build failed; jobs will fail until docker is available
)
go run ./cmd/grok-analyst --bind %GROK_ANALYST_BIND% --job-image %GROK_ANALYST_JOB_IMAGE%
endlocal
