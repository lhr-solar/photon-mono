// Package influx is the only transport between the server and InfluxDB.
package influx

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math/big"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"
)

type Client struct {
	URL, Database, Token string
	HTTP                 *http.Client
}

type APIError struct {
	Status int
	Body   string
}

func (e *APIError) Error() string { return fmt.Sprintf("Influx HTTP %d: %s", e.Status, e.Body) }
func Retryable(err error) bool {
	var api *APIError
	if errors.As(err, &api) {
		return api.Status == 429 || api.Status >= 500
	}
	var transport *url.Error
	return errors.As(err, &transport)
}

func New(base, db, token string) *Client {
	return &Client{URL: strings.TrimRight(base, "/"), Database: db, Token: token, HTTP: &http.Client{Timeout: 30 * time.Second}}
}
func (c *Client) request(ctx context.Context, path string, body []byte) ([]byte, error) {
	r, err := http.NewRequestWithContext(ctx, http.MethodPost, c.URL+path, bytes.NewReader(body))
	if err != nil {
		return nil, err
	}
	r.Header.Set("Authorization", "Bearer "+c.Token)
	r.Header.Set("Content-Type", "application/json")
	if strings.Contains(path, "write_lp") {
		r.Header.Set("Content-Type", "text/plain")
	}
	resp, err := c.HTTP.Do(r)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	b, err := io.ReadAll(io.LimitReader(resp.Body, 16*1024*1024))
	if err != nil {
		return nil, err
	}
	if resp.StatusCode >= 300 {
		return nil, &APIError{resp.StatusCode, string(b)}
	}
	return b, nil
}
func (c *Client) Write(ctx context.Context, lines []string) error {
	if len(lines) == 0 {
		return nil
	}
	_, err := c.request(ctx, "/api/v3/write_lp?db="+url.QueryEscape(c.Database)+"&precision=millisecond&accept_partial=false&no_sync=false", []byte(strings.Join(lines, "\n")))
	return err
}
func (c *Client) SQL(ctx context.Context, query string, params map[string]any) ([]map[string]any, error) {
	b, _ := json.Marshal(map[string]any{"db": c.Database, "q": query, "params": params, "format": "json"})
	out, err := c.request(ctx, "/api/v3/query_sql", b)
	if err != nil {
		return nil, err
	}
	var rows []map[string]any
	d := json.NewDecoder(bytes.NewReader(out))
	d.UseNumber()
	if err = d.Decode(&rows); err != nil {
		return nil, err
	}
	return rows, nil
}

type Query struct {
	Session, DecodeID, Signal string
	Tap                       *int32
	From, To                  uint64
	Limit, Offset             int
}
type Row struct {
	TimeMS          uint64  `json:"time_ms"`
	Signal          string  `json:"signal"`
	Tap             *int32  `json:"tap,omitempty"`
	Min             string  `json:"raw_min"`
	Max             string  `json:"raw_max"`
	Sum             string  `json:"raw_sum"`
	Count           string  `json:"count"`
	Age             *int64  `json:"age,omitempty"`
	Float           bool    `json:"float"`
	EngineeringMin  float64 `json:"min"`
	EngineeringMax  float64 `json:"max"`
	EngineeringMean float64 `json:"mean"`
	BucketMS        uint32  `json:"bucket_ms"`
	Approximate     bool    `json:"approximate"`
}

func text(v any) string {
	if v == nil {
		return ""
	}
	return fmt.Sprint(v)
}
func number(v any) float64 { n, _ := strconv.ParseFloat(text(v), 64); return n }
func (c *Client) Query(ctx context.Context, q Query) ([]Row, error) {
	if q.To < q.From || q.To-q.From > 7*24*60*60*1000 {
		return nil, fmt.Errorf("invalid history range (maximum seven days)")
	}
	if q.Limit <= 0 || q.Limit > 2000 {
		q.Limit = 2000
	}
	if q.Offset < 0 {
		return nil, fmt.Errorf("negative offset")
	}
	params := map[string]any{"session": q.Session, "decode": q.DecodeID, "from": time.UnixMilli(int64(q.From)).UTC().Format(time.RFC3339Nano), "to": time.UnixMilli(int64(q.To)).UTC().Format(time.RFC3339Nano), "limit": q.Limit, "offset": q.Offset}
	filter := "session=$session AND decode_id=$decode AND time<CAST($to AS TIMESTAMP)"
	if q.Signal != "" {
		filter += " AND signal=$signal"
		params["signal"] = q.Signal
	}
	if q.Tap != nil {
		if *q.Tap < 0 {
			filter += " AND (tap IS NULL OR tap='-1')"
		} else {
			filter += " AND tap=$tap"
			params["tap"] = strconv.Itoa(int(*q.Tap))
		}
	}
	// Both tables have stable schemas established by the writer. Integer strings avoid JSON rounding.
	columns := "time, signal, tap, min_exact, max_exact, sum_exact, count, age, has_age"
	sampleFilter := filter + " AND time>=CAST($from AS TIMESTAMP)-INTERVAL '999 millisecond'"
	legacyFilter := filter + " AND time+bucket_ms*INTERVAL '1 millisecond'>CAST($from AS TIMESTAMP)"
	query := "SELECT " + columns + ", false AS is_float,1000 AS bucket_ms,false AS approximate FROM sample WHERE " + sampleFilter + " UNION ALL SELECT " + columns + ", true AS is_float,1000 AS bucket_ms,false AS approximate FROM float_sample WHERE " + sampleFilter + " UNION ALL SELECT " + columns + ",is_float,bucket_ms,approximate FROM legacy_sample WHERE " + legacyFilter + " ORDER BY time, signal, tap LIMIT $limit OFFSET $offset"
	rows, err := c.SQL(ctx, query, params)
	if err != nil {
		return nil, err
	}
	result := make([]Row, 0, len(rows))
	for _, m := range rows {
		t, err := time.Parse(time.RFC3339Nano, text(m["time"]))
		if err != nil {
			return nil, err
		}
		r := Row{TimeMS: uint64(t.UnixMilli()), Signal: text(m["signal"]), Min: text(m["min_exact"]), Max: text(m["max_exact"]), Sum: text(m["sum_exact"]), Count: text(m["count"]), Float: m["is_float"] == true}
		r.BucketMS = uint32(number(m["bucket_ms"]))
		if r.BucketMS == 0 {
			r.BucketMS = 1000
		}
		r.Approximate = m["approximate"] == true
		if r.Min == "" {
			r.Min = text(m["min"])
		}
		if r.Max == "" {
			r.Max = text(m["max"])
		}
		if r.Sum == "" {
			r.Sum = text(m["sum"])
		}
		if v := m["tap"]; v != nil && text(v) != "" {
			n, _ := strconv.ParseInt(text(v), 10, 32)
			tap := int32(n)
			r.Tap = &tap
		}
		if v := m["age"]; v != nil && m["has_age"] == true {
			n, _ := strconv.ParseInt(text(v), 10, 64)
			r.Age = &n
		}
		r.EngineeringMin = number(r.Min)
		r.EngineeringMax = number(r.Max)
		sum, _, err := big.ParseFloat(r.Sum, 10, 256, big.ToNearestEven)
		if err != nil {
			return nil, err
		}
		f, _ := sum.Float64()
		r.EngineeringMean = f / number(r.Count)
		result = append(result, r)
	}
	return result, nil
}
