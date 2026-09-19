package analyst

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"strings"
)

const maxSummaryBytes = 16 * 1024

// Result is the sidecar payload mapped onto analysis_compute_result.
type Result struct {
	JobID    string          `json:"jobId"`
	Status   string          `json:"status"`
	Summary  string          `json:"summary,omitempty"`
	Derived  []DerivedSeries `json:"derived,omitempty"`
	Events   []Event         `json:"events,omitempty"`
	Code     string          `json:"code,omitempty"`
	Message  string          `json:"message,omitempty"`
	ClientID string          `json:"clientId,omitempty"`
}

type DerivedSeries struct {
	Name      string `json:"name"`
	Unit      string `json:"unit,omitempty"`
	PointsRef string `json:"pointsRef,omitempty"`
}

type Event struct {
	TMs     int64  `json:"tMs"`
	Name    string `json:"name,omitempty"`
	Message string `json:"message,omitempty"`
}

// Adapter runs one job inside a jail. The process sees only the bundle.
type Adapter interface {
	Start(ctx context.Context, job *Job, jail *Jail) Result
}

type RecipeAdapter struct{}

type bundleMeta struct {
	SessionID  string          `json:"sessionId"`
	DbcHash    string          `json:"dbcHash"`
	T0Ms       int64           `json:"t0Ms"`
	T1Ms       int64           `json:"t1Ms"`
	MaxPoints  int             `json:"maxPoints"`
	Prompt     string          `json:"prompt"`
	RecipeID   string          `json:"recipeId"`
	Kind       string          `json:"kind"`
	SignalIDs  []uint32        `json:"signalIds"`
	Signals    []bundleSignal  `json:"signals"`
	Threshold  *float64        `json:"threshold"`
	Series     []bundleSeries  `json:"series"`
}

type bundleSignal struct {
	ID   uint32 `json:"id"`
	Name string `json:"name"`
}

type bundleSeries struct {
	Name   string         `json:"name"`
	Unit   string         `json:"unit"`
	Points []bundlePoint  `json:"points"`
}

type bundlePoint struct {
	T int64   `json:"t"`
	V float64 `json:"v"`
}

func failResult(jobID, code, message string) Result {
	return Result{JobID: jobID, Status: "failed", Code: code, Message: message}
}

func capSummary(s string) string {
	if len(s) <= maxSummaryBytes {
		return s
	}
	return s[:maxSummaryBytes]
}

func readJailBundle(jail *Jail) (bundleMeta, int64, error) {
	var meta bundleMeta
	path, err := jail.Confine(jail.BundlePath())
	if err != nil {
		return meta, 0, err
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		return meta, 0, err
	}
	if len(raw) > maxBundleBytes {
		return meta, int64(len(raw)), errBundleCap
	}
	if err := json.Unmarshal(raw, &meta); err != nil {
		return meta, int64(len(raw)), err
	}
	return meta, int64(len(raw)), nil
}

func (RecipeAdapter) Start(ctx context.Context, job *Job, jail *Jail) Result {
	if job == nil || jail == nil {
		return failResult("", "analysis_error", "missing job jail")
	}
	if err := ctx.Err(); err != nil {
		return Result{JobID: job.ID, Status: "cancelled", Code: "analysis_cancelled", Message: "job cancelled"}
	}

	recipe := strings.TrimSpace(job.Recipe)
	if recipe == "" {
		recipe = "summarize_window"
	}
	// Scaffold: never shell out with --trust-all-tools. Freeform still only sees the bundle.
	if strings.Contains(strings.ToLower(recipe), "trust-all-tools") {
		return failResult(job.ID, "analysis_error", "--trust-all-tools is forbidden inside a job")
	}

	meta, _, err := readJailBundle(jail)
	if err != nil {
		return failResult(job.ID, "analysis_error", err.Error())
	}

	switch recipe {
	case "summarize_window", "freeform":
		return summarize(job, meta)
	case "threshold_events":
		return thresholdEvents(job, meta)
	default:
		return failResult(job.ID, "analysis_error", "unknown recipe "+recipe)
	}
}

func summarize(job *Job, meta bundleMeta) Result {
	nSig := len(meta.SignalIDs)
	if nSig == 0 {
		nSig = len(meta.Signals)
	}
	nPts := 0
	for _, s := range meta.Series {
		nPts += len(s.Points)
	}
	summary := fmt.Sprintf(
		"session %s window [%d, %d] ms signals=%d points=%d recipe=%s",
		meta.SessionID, meta.T0Ms, meta.T1Ms, nSig, nPts, job.Recipe,
	)
	if job.Recipe == "freeform" && strings.TrimSpace(meta.Prompt) != "" {
		summary += "; prompt noted (bundle only; no host FS tools)"
	}
	return Result{
		JobID:    job.ID,
		ClientID: job.ClientID,
		Status:   "succeeded",
		Summary:  capSummary(summary),
	}
}

func thresholdEvents(job *Job, meta bundleMeta) Result {
	thr := 0.0
	have := false
	if meta.Threshold != nil {
		thr = *meta.Threshold
		have = true
	}
	var events []Event
	if have {
		for _, s := range meta.Series {
			for _, p := range s.Points {
				if p.V >= thr {
					events = append(events, Event{
						TMs:     p.T,
						Name:    s.Name,
						Message: fmt.Sprintf("%s >= %g", s.Name, thr),
					})
				}
			}
		}
	}
	summary := fmt.Sprintf("threshold_events session %s events=%d", meta.SessionID, len(events))
	return Result{
		JobID:    job.ID,
		ClientID: job.ClientID,
		Status:   "succeeded",
		Summary:  capSummary(summary),
		Events:   events,
	}
}
