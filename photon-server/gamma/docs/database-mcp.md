# Database MCP interface

[database_mcp.py](../database_mcp.py) is the MCP server for InfluxDB 3. Compose runs it
as the `database-mcp` service on `127.0.0.1:9100`, and photon-server forwards the public
`/mcp` to it. Only this service receives `INFLUX_URL`, `INFLUX_DB`, and `INFLUX_TOKEN`.
The server's MCP instructions describe the SQL dialect, the time filter, and how to report
findings, so every client gets them when it connects.

| Tool | Arguments | Example result |
| --- | --- | --- |
| `list_tables` | none | `{"tables":["battery_samples"]}` |
| `describe_table` | `table: str` | `{"columns":[{"name":"voltage_v","type":"Float64"}]}` |
| `query_read_only` | `sql: str`, `max_rows: int = 100` | `{"columns":["voltage_v"],"rows":[[97.2]],"truncated":false}` |

The server only calls InfluxDB's `/api/v3/query_sql` endpoint, accepts a single `SELECT`
or `WITH` statement, stops reading after `max_rows` (at most 1000), and times out InfluxDB
requests after 30 seconds. InfluxDB 3 Core tokens are admin tokens, so these checks, not
the token, are what keep agent access read-only.

## Sign-in

With `GOOGLE_CLIENT_ID` set, FastMCP's Google provider runs the OAuth flow that MCP
clients expect: discovery under `/.well-known/`, client registration, a consent page, then
Google. Clients receive FastMCP-signed tokens; the Google tokens behind them are stored
encrypted under `FASTMCP_HOME` (`/data` in the container). Every MCP request checks the
signed-in account's verified email against `PHOTON_MCP_ALLOWED`.

Without `GOOGLE_CLIENT_ID` the server has no authentication. Compose refuses to start
without it.
