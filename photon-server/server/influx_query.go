package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"
	"sync/atomic"
	"time"
)

const (
	// influxQueryTimeout bounds one query, from sending it to reading its last row.
	influxQueryTimeout = 15 * time.Second
	// influxMaxRows is the most rows one query may return. Past it the query fails,
	// so one request cannot take all the memory.
	influxMaxRows = 300_000
	// influxErrBody is how much of an error reply's body goes into the error.
	influxErrBody = 512
)

// errNoData means InfluxDB has nothing for the query yet: no database, table or
// column that a row has written. Such a query has no rows.
var errNoData = errors.New("InfluxDB has no data for the query")

// influxQuerier runs read-only SQL against InfluxDB 3 for the history requests of
// protocol v2. Across all clients, at most a fixed number of queries run at once.
type influxQuerier struct {
	url      string // base URL, without a trailing slash
	db       string
	token    string // bearer token, or "" for none
	client   *http.Client
	sem      chan struct{} // one slot per query in flight
	failures atomic.Uint64 // queries that failed, not counting no data or a cancelled query
}

// newInfluxQuerier returns a querier for database db at baseURL, with at most
// maxConcurrent queries in flight.
func newInfluxQuerier(baseURL, db, token string, maxConcurrent int) *influxQuerier {
	return &influxQuerier{
		url:    strings.TrimRight(baseURL, "/"),
		db:     db,
		token:  token,
		client: &http.Client{Timeout: influxQueryTimeout},
		sem:    make(chan struct{}, max(maxConcurrent, 1)),
	}
}

// query runs sql, with its $name parameters, and returns its rows, each a map from
// column name to value. Numbers are json.Number, so 64-bit times stay exact. It
// waits for a free slot and gives up when ctx ends. A query with no data returns
// errNoData.
func (q *influxQuerier) query(ctx context.Context, sql string, params map[string]string) ([]map[string]any, error) {
	select {
	case q.sem <- struct{}{}:
	case <-ctx.Done():
		return nil, ctx.Err()
	}
	defer func() { <-q.sem }()

	rows, err := q.run(ctx, sql, params)
	if err != nil && !errors.Is(err, errNoData) && ctx.Err() == nil {
		q.failures.Add(1)
	}
	return rows, err
}

// run posts one query and reads its rows, one JSON object per line.
func (q *influxQuerier) run(ctx context.Context, sql string, params map[string]string) ([]map[string]any, error) {
	if params == nil {
		params = map[string]string{}
	}
	body, err := json.Marshal(map[string]any{"db": q.db, "q": sql, "params": params, "format": "jsonl"})
	if err != nil {
		return nil, err
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, q.url+"/api/v3/query_sql", bytes.NewReader(body))
	if err != nil {
		return nil, err
	}
	req.Header.Set("Content-Type", "application/json")
	if q.token != "" {
		req.Header.Set("Authorization", "Bearer "+q.token)
	}
	resp, err := q.client.Do(req)
	if err != nil {
		return nil, fmt.Errorf("InfluxDB query: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		msg, _ := io.ReadAll(io.LimitReader(resp.Body, influxErrBody))
		text := strings.TrimSpace(string(msg))
		if noDataReply(resp.StatusCode, text) {
			return nil, errNoData
		}
		return nil, fmt.Errorf("InfluxDB query: HTTP %d: %s", resp.StatusCode, text)
	}

	rows := []map[string]any{}
	dec := json.NewDecoder(resp.Body)
	dec.UseNumber()
	for {
		var row map[string]any
		err := dec.Decode(&row)
		if err == io.EOF {
			return rows, nil // an empty body is a query with no rows
		}
		if err != nil {
			return nil, fmt.Errorf("InfluxDB query: bad row: %w", err)
		}
		if len(rows) == influxMaxRows {
			return nil, fmt.Errorf("InfluxDB query: more than %d rows", influxMaxRows)
		}
		rows = append(rows, row)
	}
}

// noDataReply reports whether an error reply means nothing has been written for
// the query: the database does not exist (404), or a table or column is missing
// because no row has created it. InfluxDB answers those with 400, 404 or 500,
// depending on which one is missing.
func noDataReply(status int, text string) bool {
	switch {
	case status == http.StatusNotFound && strings.Contains(text, "database not found"):
		return true
	case strings.Contains(text, "No field named"):
		return true
	case strings.Contains(text, "table '") && strings.Contains(text, "not found"):
		return true
	}
	return false
}

// rowInt reads an integer column of a row.
func rowInt(row map[string]any, key string) (int64, error) {
	n, ok := row[key].(json.Number)
	if !ok {
		return 0, fmt.Errorf("InfluxDB row has no number %q", key)
	}
	return n.Int64()
}

// rowFloat reads a numeric column of a row as a float.
func rowFloat(row map[string]any, key string) (float64, error) {
	n, ok := row[key].(json.Number)
	if !ok {
		return 0, fmt.Errorf("InfluxDB row has no number %q", key)
	}
	return n.Float64()
}

// rowString reads a text column of a row.
func rowString(row map[string]any, key string) (string, error) {
	s, ok := row[key].(string)
	if !ok {
		return "", fmt.Errorf("InfluxDB row has no text %q", key)
	}
	return s, nil
}
