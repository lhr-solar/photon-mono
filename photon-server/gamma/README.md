# Photon MCP

[database_mcp.py](database_mcp.py) gives AI agents read-only SQL access to the car's
telemetry in InfluxDB 3. Team members connect their own agent (Claude, Claude Code, Codex)
and sign in with Google, so their own plan pays for the model.

## Connect your agent

Click **Agent link** in Photon's titlebar. It copies `https://150.136.85.207.sslip.io/mcp`.

- **Claude** (claude.ai, Claude Desktop, and Claude Code signed in to the same account):
  Settings → Connectors → Add custom connector, paste the link, then Connect. On Team and
  Enterprise plans an owner may have to add it.
- **Claude Code:** `claude mcp add --transport http photon <link>`, then `/mcp` →
  photon → Authenticate.
- **Codex:** `codex mcp add photon --url <link>`. If no browser opens, run
  `codex mcp login photon`.

A Photon consent page opens, then Google. Sign in with an account on the allowlist. The
agent gets `list_tables`, `describe_table`, and `query_read_only`, plus the server's notes on
the SQL dialect. Query results go to your agent's model provider.

## Run it on the PC

1. In the Google Cloud console, set up Google sign-in (Google Auth Platform):
   - Create an OAuth client of type **Web application** with the authorized redirect URI
     `https://150.136.85.207.sslip.io/auth/callback`.
   - Publish the app. In Testing mode only the listed test users can sign in. The server
     asks only for `openid` and `email`, which need no Google review.
2. Copy the settings from [.env.example](.env.example) into `.env`: the client ID and
   secret, `PHOTON_MCP_ALLOWED`, and the InfluxDB URL and token.
3. Start it. `--remove-orphans` also removes the old `gamma` chatbot container.

   ```bash
   docker compose up --build -d --remove-orphans
   ```

photon-server forwards `/mcp` and the sign-in paths (`/authorize`, `/token`, `/register`,
`/consent`, `/auth/callback`, `/.well-known/…`) to `127.0.0.1:9100`, or to
`PHOTON_MCP_URL`. Those paths skip the console password; database-mcp checks every MCP
request itself.

`PHOTON_MCP_ALLOWED` lists Google accounts (`name@gmail.com`) and whole domains
(`@utexas.edu`). It is checked on every request, so removing someone takes effect once
`docker compose up -d` restarts the service. Sign-in state lives in the `sign-in` volume;
`docker compose down -v` signs everyone out.

The container is non-root with a read-only filesystem, resource limits, and a loopback
port. [The MCP interface](docs/database-mcp.md) describes the tools and their limits.

## Develop

```bash
uv sync --frozen
uv run python database_mcp.py   # :9000; without Google settings it is open, so keep it on loopback
uv run pytest
```

## Agents in this repo

[`/.mcp.json`](../../.mcp.json) (Claude Code) and [`/.codex/config.toml`](../../.codex/config.toml)
(Codex) connect agents started in this checkout to two servers:

- `photon-db`: this server at its public address. Sign in on first use.
- `mermaid`: [mcp-mermaid](https://github.com/hustcc/mcp-mermaid) 0.4.1, run per session
  with `docker run` over stdio and no network. Build it once with
  `docker build -t photon-mermaid-mcp:0.4.1 mermaid` (about 2 GB). Its `svg_url`/`png_url`
  outputs are public mermaid.ink links; opening one sends the diagram there.
