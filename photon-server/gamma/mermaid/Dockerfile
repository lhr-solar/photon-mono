# Mermaid MCP server (hustcc/mcp-mermaid) for local agents such as Claude Code, which
# runs it over stdio with no network (see /.mcp.json). Rendering is local: the browser
# is installed by the same Playwright that mcp-mermaid resolved, so the versions match.
FROM node:22-bookworm-slim

ENV PLAYWRIGHT_BROWSERS_PATH=/ms-playwright \
    NODE_ENV=production \
    npm_config_update_notifier=false

WORKDIR /app
RUN npm install --omit=dev mcp-mermaid@0.4.1 \
    && npx playwright install --with-deps chromium \
    && rm -rf /root/.npm /var/lib/apt/lists/*

USER node
# The container runs read-only; Chromium and fontconfig write their caches to the /tmp tmpfs.
ENV HOME=/tmp
ENTRYPOINT ["node", "/app/node_modules/mcp-mermaid/build/index.js"]
