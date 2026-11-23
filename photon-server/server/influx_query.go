package main

import (
	"bytes"
	"context"
	"encoding/csv"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"slices"
	"strings"
	"sync/atomic"
	"time"
)

const (
	// influxQueryTimeout bounds one query, from sending it to reading its last row.
	influxQueryTimeout = 15 * time.Second
	// influxTileTimeout bounds one tile query, from sending it to reading its last
	// row. A tile is read at the client's pace, so it gets longer than a plain query.
	influxTileTimeout = 60 * time.Second
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
	token    string        // bearer token, or "" for none
	client   *http.Client  // for query: the whole query is within influxQueryTimeout
	streamer *http.Client  // for stream: no overall timeout; the query's context sets its deadline
	sem      chan struct{} // one slot per query in flight
	failures atomic.Uint64 // queries that failed, not counting no data or a cancelled query
}

// newInfluxQuerier returns a querier for database db at baseURL, with at most
// maxConcurrent queries in flight.
func newInfluxQuerier(baseURL, db, token string, maxConcurrent int) *influxQuerier {
	return &influxQuerier{
		url:      strings.TrimRight(baseURL, "/"),
		db:       db,
		token:    token,
		client:   &http.Client{Timeout: influxQueryTimeout},
		streamer: &http.Client{},
		sem:      make(chan struct{}, max(maxConcurrent, 1)),
	}
}

// acquire waits for a query slot, and gives up when ctx ends. The caller gives the
// slot back by calling the function it returns.
func (q *influxQuerier) acquire(ctx context.Context) (func(), error) {
	select {
	case q.sem <- struct{}{}:
		return func() { <-q.sem }, nil
	case <-ctx.Done():
		return nil, ctx.Err()
	}
}

// query runs sql, with its $name parameters, and returns its rows, each a map from
// column name to value. Numbers are json.Number, so 64-bit times stay exact. It
// waits for a free slot and gives up when ctx ends. A query with no data returns
// errNoData.
func (q *influxQuerier) query(ctx context.Context, sql string, params map[string]string) ([]map[string]any, error) {
	release, err := q.acquire(ctx)
	if err != nil {
		return nil, err
	}
	defer release()

	rows, err := q.run(ctx, sql, params)
	if err != nil && !errors.Is(err, errNoData) && ctx.Err() == nil {
		q.failures.Add(1)
	}
	return rows, err
}

// stream runs sql like query, but reads the reply as CSV, one row at a time, and
// hands each row to fn. The first line of the reply names the columns, which must
// be cols in that order. Each later line gives fn its fields in that order, and a
// NULL is "". The rows are not kept, so memory does not grow with the reply. The
// read goes at fn's pace, and the query gives up after influxTileTimeout. An error
// from fn stops the read and is returned as it is. fn must not keep its row.
func (q *influxQuerier) stream(ctx context.Context, sql string, params map[string]string, cols []string, fn func(row []string) error) error {
	release, err := q.acquire(ctx)
	if err != nil {
		return err
	}
	defer release()

	qctx, cancel := context.WithTimeout(ctx, influxTileTimeout)
	defer cancel()
	err = q.readCSV(qctx, sql, params, cols, fn)
	if err != nil && !errors.Is(err, errNoData) && ctx.Err() == nil {
		q.failures.Add(1)
	}
	return err
}

// run posts one query and reads its rows, one JSON object per line.
func (q *influxQuerier) run(ctx context.Context, sql string, params map[string]string) ([]map[string]any, error) {
	resp, err := q.post(ctx, q.client, sql, params, "jsonl")
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()

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

// readCSV posts sql with the CSV format and hands the rows of the reply to fn, as
// stream describes.
func (q *influxQuerier) readCSV(ctx context.Context, sql string, params map[string]string, cols []string, fn func(row []string) error) error {
	resp, err := q.post(ctx, q.streamer, sql, params, "csv")
	if err != nil {
		return err
	}
	defer resp.Body.Close()

	r := csv.NewReader(resp.Body)
	r.ReuseRecord = true // every row reuses one slice
	header, err := r.Read()
	if err == io.EOF {
		return nil // an empty body is a query with no rows, and no header
	}
	if err != nil {
		return fmt.Errorf("InfluxDB query: bad header: %w", err)
	}
	if !slices.Equal(header, cols) {
		return fmt.Errorf("InfluxDB query: columns are %q, want %q", header, cols)
	}
	// The header sets the field count, so every row has as many fields as cols.
	for {
		row, err := r.Read()
		if err == io.EOF {
			return nil
		}
		if err != nil {
			return fmt.Errorf("InfluxDB query: bad row: %w", err)
		}
		if err := fn(row); err != nil {
			return err
		}
	}
}

// post sends sql and its parameters, asking for the reply in format. It returns the
// reply when it is 200 OK. Any other reply is an error: errNoData when it means
// nothing has been written for the query yet. The caller closes the body of a reply
// it gets.
func (q *influxQuerier) post(ctx context.Context, client *http.Client, sql string, params map[string]string, format string) (*http.Response, error) {
	if params == nil {
		params = map[string]string{}
	}
	body, err := json.Marshal(map[string]any{"db": q.db, "q": sql, "params": params, "format": format})
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
	resp, err := client.Do(req)
	if err != nil {
		return nil, fmt.Errorf("InfluxDB query: %w", err)
	}
	if resp.StatusCode == http.StatusOK {
		return resp, nil
	}
	defer resp.Body.Close()
	msg, _ := io.ReadAll(io.LimitReader(resp.Body, influxErrBody))
	text := strings.TrimSpace(string(msg))
	if noDataReply(resp.StatusCode, text) {
		return nil, errNoData
	}
	return nil, fmt.Errorf("InfluxDB query: HTTP %d: %s", resp.StatusCode, text)
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
