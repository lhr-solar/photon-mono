package analyst

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

const (
	defaultBind     = "127.0.0.1:18765"
	defaultTimeoutS = 120
	maxInflightPer  = 1
	maxInflightAll  = 2
	maxJSONBody     = 64 * 1024
)

type JobRequest struct {
	JobID       string `json:"jobId"`
	ClientID    string `json:"clientId"`
	BundlePath  string `json:"bundlePath"`
	Recipe      string `json:"recipe"`
	TimeoutS    int    `json:"timeoutS"`
	SessionID   string `json:"sessionId,omitempty"`
	T0Ms        int64  `json:"t0Ms,omitempty"`
	T1Ms        int64  `json:"t1Ms,omitempty"`
	SignalCount int    `json:"signalCount,omitempty"`
	Prompt      string `json:"prompt,omitempty"`
}

type Job struct {
	ID          string
	ClientID    string
	SessionID   string
	Recipe      string
	BundlePath  string
	TimeoutS    int
	T0Ms        int64
	T1Ms        int64
	SignalCount int
	Prompt      string

	mu      sync.Mutex
	status  string
	result  Result
	bytesIn int64
	started time.Time
	cancel  context.CancelFunc
	jail    *Jail
}

func (j *Job) snapshot() Result {
	j.mu.Lock()
	defer j.mu.Unlock()
	out := j.result
	if out.JobID == "" {
		out.JobID = j.ID
	}
	if out.ClientID == "" {
		out.ClientID = j.ClientID
	}
	if out.Status == "" {
		out.Status = j.status
	}
	return out
}

func (j *Job) setStatus(status string, res Result) {
	j.mu.Lock()
	j.status = status
	if res.JobID == "" {
		res.JobID = j.ID
	}
	if res.Status == "" {
		res.Status = status
	}
	j.result = res
	j.mu.Unlock()
}

func (j *Job) destroyJail() {
	if j == nil {
		return
	}
	j.mu.Lock()
	jail := j.jail
	j.jail = nil
	j.mu.Unlock()
	if jail != nil {
		_ = jail.Destroy()
	}
}

type Config struct {
	Bind    string
	Scratch string
	Audit   string
	Adapter Adapter
}

func DefaultScratch() string {
	return filepath.Join(os.TempDir(), "grok-analyst")
}

func DefaultAudit(scratch string) string {
	return filepath.Join(scratch, "audit.log")
}

type Server struct {
	cfg     Config
	http    *http.Server
	ln      net.Listener
	audit   *AuditLog
	adapter Adapter

	mu   sync.Mutex
	jobs map[string]*Job
}

func isLoopbackHost(host string) bool {
	h := strings.TrimSpace(host)
	if h == "" || h == "127.0.0.1" || h == "localhost" || h == "::1" {
		return true
	}
	ip := net.ParseIP(h)
	return ip != nil && ip.IsLoopback()
}

func ParseBind(bind string) (string, error) {
	raw := strings.TrimSpace(bind)
	if raw == "" {
		raw = defaultBind
	}
	host, port, err := net.SplitHostPort(raw)
	if err != nil {
		return "", fmt.Errorf("bind must be 127.0.0.1:port: %w", err)
	}
	if !isLoopbackHost(host) {
		return "", fmt.Errorf("grok-analyst binds loopback only (got %s)", host)
	}
	if host == "localhost" || host == "::1" {
		host = "127.0.0.1"
	}
	return net.JoinHostPort(host, port), nil
}

func NewServer(cfg Config) (*Server, error) {
	bind, err := ParseBind(cfg.Bind)
	if err != nil {
		return nil, err
	}
	cfg.Bind = bind
	if strings.TrimSpace(cfg.Scratch) == "" {
		cfg.Scratch = DefaultScratch()
	}
	if looksLikeStore(cfg.Scratch) {
		return nil, fmt.Errorf("scratch must not be a TSDB or video store")
	}
	if strings.TrimSpace(cfg.Audit) == "" {
		cfg.Audit = DefaultAudit(cfg.Scratch)
	}
	if err := os.MkdirAll(cfg.Scratch, 0o700); err != nil {
		return nil, err
	}
	audit, err := NewAuditLog(cfg.Audit)
	if err != nil {
		return nil, err
	}
	ad := cfg.Adapter
	if ad == nil {
		ad = RecipeAdapter{}
	}
	s := &Server{
		cfg:     cfg,
		audit:   audit,
		adapter: ad,
		jobs:    make(map[string]*Job),
	}
	mux := http.NewServeMux()
	mux.HandleFunc("POST /jobs", s.handlePostJob)
	mux.HandleFunc("GET /jobs/{id}", s.handleGetJob)
	mux.HandleFunc("POST /jobs/{id}/cancel", s.handleCancel)
	s.http = &http.Server{
		Addr:              cfg.Bind,
		Handler:           mux,
		ReadHeaderTimeout: 2 * time.Second,
		ReadTimeout:       5 * time.Second,
		WriteTimeout:      5 * time.Second,
		MaxHeaderBytes:    16 * 1024,
	}
	return s, nil
}

func (s *Server) Start() error {
	ln, err := net.Listen("tcp", s.cfg.Bind)
	if err != nil {
		return err
	}
	// Belt: refuse a listener that somehow landed off loopback.
	addr := ln.Addr().(*net.TCPAddr)
	if addr == nil || !addr.IP.IsLoopback() {
		_ = ln.Close()
		return fmt.Errorf("listener is not loopback: %s", ln.Addr())
	}
	s.ln = ln
	s.http.Addr = ln.Addr().String()
	go func() {
		if err := s.http.Serve(ln); err != nil && !errors.Is(err, http.ErrServerClosed) {
			log.Printf("[grok-analyst] http: %v", err)
		}
	}()
	log.Printf("[grok-analyst] listening on http://%s (loopback only)", ln.Addr())
	log.Printf("[grok-analyst] scratch %s", s.cfg.Scratch)
	log.Printf("[grok-analyst] audit %s", s.audit.Path())
	return nil
}

func (s *Server) Addr() string {
	if s.ln != nil {
		return s.ln.Addr().String()
	}
	return s.cfg.Bind
}

func (s *Server) Close() error {
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	s.CancelAll()
	if s.http != nil {
		_ = s.http.Shutdown(ctx)
	}
	if s.ln != nil {
		_ = s.ln.Close()
	}
	return nil
}

func (s *Server) CancelAll() {
	s.mu.Lock()
	jobs := make([]*Job, 0, len(s.jobs))
	for _, j := range s.jobs {
		jobs = append(jobs, j)
	}
	s.mu.Unlock()
	for _, j := range jobs {
		s.cancelJob(j)
	}
}

func readJobRequest(r *http.Request) (JobRequest, error) {
	var req JobRequest
	body, err := io.ReadAll(io.LimitReader(r.Body, maxJSONBody+1))
	if err != nil {
		return req, err
	}
	if len(body) > maxJSONBody {
		return req, fmt.Errorf("request body too large")
	}
	if len(body) == 0 {
		return req, fmt.Errorf("empty body")
	}
	if err := json.Unmarshal(body, &req); err != nil {
		return req, err
	}
	return req, nil
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

func (s *Server) handlePostJob(w http.ResponseWriter, r *http.Request) {
	req, err := readJobRequest(r)
	if err != nil {
		writeJSON(w, http.StatusBadRequest, failResult(req.JobID, "bad_message", err.Error()))
		return
	}
	if !safeID(req.JobID) || !safeID(req.ClientID) {
		writeJSON(w, http.StatusBadRequest, failResult(req.JobID, "bad_message", errBadID.Error()))
		return
	}
	if looksLikeStore(req.BundlePath) {
		writeJSON(w, http.StatusBadRequest, failResult(req.JobID, "analysis_error", errTSDBPath.Error()))
		return
	}
	timeout := req.TimeoutS
	if timeout <= 0 {
		timeout = defaultTimeoutS
	}

	s.mu.Lock()
	if existing, ok := s.jobs[req.JobID]; ok {
		s.mu.Unlock()
		writeJSON(w, http.StatusOK, existing.snapshot())
		return
	}
	clientBusy := 0
	for _, j := range s.jobs {
		if j.ClientID == req.ClientID && (j.snapshot().Status == "queued" || j.snapshot().Status == "running") {
			clientBusy++
		}
	}
	running := 0
	for _, j := range s.jobs {
		st := j.snapshot().Status
		if st == "queued" || st == "running" {
			running++
		}
	}
	if clientBusy >= maxInflightPer || running >= maxInflightAll {
		s.mu.Unlock()
		writeJSON(w, http.StatusServiceUnavailable, failResult(req.JobID, "analysis_busy", "inflight limit reached"))
		return
	}

	jail, err := NewJail(s.cfg.Scratch, req.ClientID, req.JobID)
	if err != nil {
		s.mu.Unlock()
		writeJSON(w, http.StatusBadRequest, failResult(req.JobID, "analysis_error", err.Error()))
		return
	}
	var n int64
	if strings.TrimSpace(req.BundlePath) == "" {
		meta, _ := json.Marshal(map[string]any{
			"sessionId":   req.SessionID,
			"t0Ms":        req.T0Ms,
			"t1Ms":        req.T1Ms,
			"recipeId":    req.Recipe,
			"prompt":      req.Prompt,
			"signalCount": req.SignalCount,
		})
		if err := os.WriteFile(jail.BundlePath(), meta, 0o400); err != nil {
			_ = jail.Destroy()
			s.mu.Unlock()
			writeJSON(w, http.StatusBadRequest, failResult(req.JobID, "analysis_error", err.Error()))
			return
		}
		n = int64(len(meta))
	} else {
		var err error
		n, err = jail.InstallBundle(req.BundlePath)
		if err != nil {
			_ = jail.Destroy()
			s.mu.Unlock()
			writeJSON(w, http.StatusBadRequest, failResult(req.JobID, "analysis_error", err.Error()))
			return
		}
	}

	ctx, cancel := context.WithTimeout(context.Background(), time.Duration(timeout)*time.Second)
	job := &Job{
		ID:          req.JobID,
		ClientID:    req.ClientID,
		SessionID:   req.SessionID,
		Recipe:      req.Recipe,
		BundlePath:  req.BundlePath,
		TimeoutS:    timeout,
		T0Ms:        req.T0Ms,
		T1Ms:        req.T1Ms,
		SignalCount: req.SignalCount,
		Prompt:      req.Prompt,
		status:      "queued",
		started:     time.Now(),
		cancel:      cancel,
		jail:        jail,
		bytesIn:     n,
		result:      Result{JobID: req.JobID, ClientID: req.ClientID, Status: "queued"},
	}
	s.jobs[job.ID] = job
	s.mu.Unlock()

	go s.run(ctx, job)
	writeJSON(w, http.StatusAccepted, job.snapshot())
}

func (s *Server) run(ctx context.Context, job *Job) {
	job.setStatus("running", Result{JobID: job.ID, ClientID: job.ClientID, Status: "running"})
	res := s.adapter.Start(ctx, job, job.jail)
	if ctx.Err() != nil && res.Status != "cancelled" && res.Status != "failed" {
		if errors.Is(ctx.Err(), context.DeadlineExceeded) {
			res = failResult(job.ID, "analysis_timeout", "job timed out")
		} else {
			res = Result{JobID: job.ID, Status: "cancelled", Code: "analysis_cancelled", Message: "job cancelled"}
		}
	}
	if res.Status == "" {
		res.Status = "failed"
	}
	job.setStatus(res.Status, res)
	elapsed := time.Since(job.started).Milliseconds()
	_ = s.audit.Append(AuditRow{
		ClientID:    job.ClientID,
		JobID:       job.ID,
		SessionID:   job.SessionID,
		T0Ms:        job.T0Ms,
		T1Ms:        job.T1Ms,
		SignalCount: job.SignalCount,
		Recipe:      job.Recipe,
		Status:      res.Status,
		ElapsedMs:   elapsed,
		BytesIn:     job.bytesIn,
		BytesOut:    int64(len(res.Summary)),
	})
	job.destroyJail()
}

func (s *Server) lookup(id string) *Job {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.jobs[id]
}

func (s *Server) handleGetJob(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	job := s.lookup(id)
	if job == nil {
		writeJSON(w, http.StatusNotFound, failResult(id, "not_found", "unknown job"))
		return
	}
	writeJSON(w, http.StatusOK, job.snapshot())
}

func (s *Server) handleCancel(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	job := s.lookup(id)
	if job == nil {
		writeJSON(w, http.StatusNotFound, failResult(id, "not_found", "unknown job"))
		return
	}
	s.cancelJob(job)
	writeJSON(w, http.StatusOK, job.snapshot())
}

func (s *Server) cancelJob(job *Job) {
	if job == nil {
		return
	}
	if job.cancel != nil {
		job.cancel()
	}
	st := job.snapshot().Status
	if st == "queued" || st == "running" {
		job.setStatus("cancelled", Result{
			JobID:    job.ID,
			ClientID: job.ClientID,
			Status:   "cancelled",
			Code:     "analysis_cancelled",
			Message:  "job cancelled",
		})
	}
	job.destroyJail()
}

func (s *Server) JailDir(clientID, jobID string) string {
	job := s.lookup(jobID)
	if job == nil || job.jail == nil {
		return filepath.Join(s.cfg.Scratch, "jobs", clientID, jobID)
	}
	return job.jail.Dir
}
