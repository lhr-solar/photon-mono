FROM ghcr.io/astral-sh/uv:0.8.22 AS uv
FROM python:3.12-slim

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    UV_LINK_MODE=copy \
    UV_CACHE_DIR=/tmp/uv-cache \
    PATH="/app/.venv/bin:$PATH" \
    FASTMCP_HOME=/data

COPY --from=uv /uv /uvx /usr/local/bin/
WORKDIR /app
COPY pyproject.toml uv.lock ./
# /data holds sign-in state (client registrations, encrypted Google tokens). A named
# volume mounted there starts out owned by the service user.
RUN uv sync --frozen --no-dev --no-install-project \
    && rm -rf /tmp/uv-cache \
    && groupadd --gid 10001 photon \
    && useradd --uid 10001 --gid photon --no-create-home photon \
    && mkdir /data && chown 10001:10001 /data
COPY database_mcp.py ./
USER 10001:10001
EXPOSE 9000
HEALTHCHECK --interval=30s --timeout=3s --start-period=15s --retries=3 \
    CMD python -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:9000/healthz', timeout=2)"
CMD ["python", "database_mcp.py"]
