// grok-job runs inside a per-job container. It may only read /job
// (the bind-mounted scratch for this client/job). No TSDB, no video,
// no docker.sock, no repo, no host tools.
package main

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"
	"time"
)

const (
	jobRoot         = "/job"
	maxSummaryBytes = 16 << 10
	maxBundleBytes  = 32 << 20
)

type request struct {
	JobID       string `json:"jobId"`
	ClientID    string `json:"clientId"`
	Recipe      string `json:"recipe"`
	TimeoutS    int    `json:"timeoutS"`
	SessionID   string `json:"sessionId"`
	T0          int64  `json:"t0"`
	T1          int64  `json:"t1"`
	SignalCount int    `json:"signalCount"`
	Prompt      string `json:"prompt"`
	HoldMs      int    `json:"holdMs"`
}

type bundleMeta struct {
	SessionID string         `json:"sessionId"`
	T0Ms      int64          `json:"t0Ms"`
	T1Ms      int64          `json:"t1Ms"`
	Prompt    string         `json:"prompt"`
	RecipeID  string         `json:"recipeId"`
	SignalIDs []uint32       `json:"signalIds"`
	Signals   []bundleSignal `json:"signals"`
	Threshold *float64       `json:"threshold"`
	Series    []bundleSeries `json:"series"`
}

type bundleSignal struct {
	ID   uint32 `json:"id"`
	Name string `json:"name"`
}

type bundleSeries struct {
	Name   string        `json:"name"`
	Unit   string        `json:"unit"`
	Points []bundlePoint `json:"points"`
}

type bundlePoint struct {
	T int64   `json:"t"`
	V float64 `json:"v"`
}

type result struct {
	Status  string  `json:"status"`
	Summary string  `json:"summary,omitempty"`
	Code    string  `json:"code,omitempty"`
	Message string  `json:"message,omitempty"`
	Events  []event `json:"events,omitempty"`
}

type event struct {
	TMs     int64  `json:"tMs"`
	Name    string `json:"name,omitempty"`
	Message string `json:"message,omitempty"`
}

func main() {
	os.Exit(run())
}

func run() int {
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGTERM, syscall.SIGINT)
	defer stop()

	req, err := readRequest()
	if err != nil {
		writeResult(result{Status: "failed", Code: "container", Message: err.Error()})
		return 1
	}
	if strings.Contains(strings.ToLower(req.Recipe), "trust-all-tools") {
		writeResult(result{Status: "failed", Code: "analysis_error", Message: "--trust-all-tools is forbidden inside a job"})
		return 1
	}

	if req.HoldMs > 0 {
		timer := time.NewTimer(time.Duration(req.HoldMs) * time.Millisecond)
		defer timer.Stop()
		select {
		case <-ctx.Done():
			writeResult(result{Status: "cancelled", Code: "cancelled", Message: "job cancelled"})
			return 0
		case <-timer.C:
		}
	}

	select {
	case <-ctx.Done():
		writeResult(result{Status: "cancelled", Code: "cancelled", Message: "job cancelled"})
		return 0
	default:
	}

	res := runRecipe(req)
	writeResult(res)
	if res.Status == "succeeded" {
		return 0
	}
	return 1
}

func readRequest() (request, error) {
	var req request
	path, err := confine(filepath.Join(jobRoot, "request.json"))
	if err != nil {
		return req, err
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		return req, fmt.Errorf("request.json: %w", err)
	}
	if err := json.Unmarshal(raw, &req); err != nil {
		return req, fmt.Errorf("request.json: %w", err)
	}
	if req.Recipe == "" {
		req.Recipe = "summarize_window"
	}
	return req, nil
}

func runRecipe(req request) result {
	meta, err := readBundle()
	if err != nil && !os.IsNotExist(err) {
		return result{Status: "failed", Code: "analysis_error", Message: err.Error()}
	}
	session := req.SessionID
	t0, t1 := req.T0, req.T1
	if meta.SessionID != "" {
		session = meta.SessionID
	}
	if meta.T0Ms != 0 || meta.T1Ms != 0 {
		t0, t1 = meta.T0Ms, meta.T1Ms
	}
	nSig := req.SignalCount
	if len(meta.SignalIDs) > 0 {
		nSig = len(meta.SignalIDs)
	} else if len(meta.Signals) > 0 {
		nSig = len(meta.Signals)
	}

	switch req.Recipe {
	case "summarize_window", "freeform":
		nPts := 0
		for _, s := range meta.Series {
			nPts += len(s.Points)
		}
		summary := fmt.Sprintf("session %s window [%d, %d] ms signals=%d points=%d recipe=%s",
			session, t0, t1, nSig, nPts, req.Recipe)
		if req.Recipe == "freeform" {
			prompt := req.Prompt
			if prompt == "" {
				prompt = meta.Prompt
			}
			if strings.TrimSpace(prompt) != "" {
				summary += "; prompt noted (bundle only; no host FS tools)"
			}
		}
		return result{Status: "succeeded", Summary: capSummary(summary)}
	case "threshold_events":
		var events []event
		if meta.Threshold != nil {
			thr := *meta.Threshold
			for _, s := range meta.Series {
				for _, p := range s.Points {
					if p.V >= thr {
						events = append(events, event{
							TMs:     p.T,
							Name:    s.Name,
							Message: fmt.Sprintf("%s >= %g", s.Name, thr),
						})
					}
				}
			}
		}
		summary := fmt.Sprintf("threshold_events session %s events=%d", session, len(events))
		return result{Status: "succeeded", Summary: capSummary(summary), Events: events}
	default:
		return result{Status: "failed", Code: "analysis_error", Message: "unknown recipe " + req.Recipe}
	}
}

func readBundle() (bundleMeta, error) {
	var meta bundleMeta
	path, err := confine(filepath.Join(jobRoot, "bundle.json"))
	if err != nil {
		return meta, err
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		return meta, err
	}
	if len(raw) > maxBundleBytes {
		return meta, fmt.Errorf("bundle exceeds 32 MB cap")
	}
	if err := json.Unmarshal(raw, &meta); err != nil {
		return meta, err
	}
	return meta, nil
}

func writeResult(res result) {
	path, err := confine(filepath.Join(jobRoot, "result.json"))
	if err != nil {
		return
	}
	raw, err := json.Marshal(res)
	if err != nil {
		return
	}
	_ = os.WriteFile(path, raw, 0o644)
}

func capSummary(s string) string {
	if len(s) <= maxSummaryBytes {
		return s
	}
	return s[:maxSummaryBytes]
}

func confine(path string) (string, error) {
	root, err := filepath.Abs(jobRoot)
	if err != nil {
		return "", err
	}
	abs, err := filepath.Abs(path)
	if err != nil {
		return "", err
	}
	rel, err := filepath.Rel(root, abs)
	if err != nil || rel == ".." || strings.HasPrefix(rel, ".."+string(os.PathSeparator)) || strings.HasPrefix(rel, "../") {
		return "", fmt.Errorf("path is outside the job jail")
	}
	return abs, nil
}
