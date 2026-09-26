// Package analyst is the WP-H grok-analyst sidecar: loopback HTTP, one
// container per job, controlled mounts, and an append-only audit log.
// Job containers never see docker.sock, TSDB, video, or the repo.
package analyst

import (
	"bytes"
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
	DefaultBind     = "127.0.0.1:18765"
	DefaultTimeoutS = 120
	MaxGlobalJobs   = 2
	MaxClientJobs   = 1
	MaxBundleBytes  = 32 << 20
	MaxSummaryBytes = 16 << 10
)

var (
	errOfflinePorts = map[string]struct{}{
		"6100": {},
		"6500": {},
		"6700": {},
	}
)

// JobRequest is POST /jobs. BundlePath is recorded only; the sidecar does
// not open it (backend owns history slices). Forbidden host mounts stay out.
type JobRequest struct {
	JobID       string `json:"jobId"`
	ClientID    string `json:"clientId"`
	BundlePath  string `json:"bundlePath"`
	Recipe      string `json:"recipe"`
	TimeoutS    int    `json:"timeoutS"`
	SessionID   string `json:"sessionId"`
	T0          int64  `json:"t0"`
	T1          int64  `json:"t1"`
	SignalCount int    `json:"signalCount"`
	Prompt      string `json:"prompt,omitempty"`
	// HoldMs is a test-only hook so cancel/timeout can observe a running container.
	HoldMs int `json:"holdMs,omitempty"`
}

type JobView struct {
	JobID          string `json:"jobId"`
	ClientID       string `json:"clientId"`
	Status         string `json:"status"`
	Recipe         string `json:"recipe"`
	SessionID      string `json:"sessionId"`
	Summary        string `json:"summary,omitempty"`
	Code           string `json:"code,omitempty"`
	Message        string `json:"message,omitempty"`
	JailPath       string `json:"jailPath,omitempty"`
	JailAlive      bool   `json:"jailAlive"`
	Container      string `json:"container,omitempty"`
	ContainerAlive bool   `json:"containerAlive"`
	ElapsedMs      int64  `json:"elapsedMs"`
	BytesIn        int64  `json:"bytesIn"`
	BytesOut       int64  `json:"bytesOut"`
}

type job struct {
	req       JobRequest
	status    string
	summary   string
	code      string
	message   string
	jail      string
	container string
	logs      string
	started   time.Time
	elapsedMs int64
	bytesIn   int64
	bytesOut  int64
	cancel    context.CancelFunc
	done      chan struct{}
	closing   bool
}

// Sidecar is the loopback job runner. Jobs execute in per-job containers;
// the host scratch dir is only the bind-mount source for that job.
type Sidecar struct {
	Bind      string
	Scratch   string
	AuditPath string
	DockerBin string
	JobImage  string

	mu     sync.Mutex
	jobs   map[string]*job // keyed by clientId + "\x1f" + jobId
	srv    *http.Server
	ln     net.Listener
	closed bool
}

func New(bind, scratch, auditPath string) *Sidecar {
	if bind == "" {
		bind = DefaultBind
	}
	return &Sidecar{
		Bind:      bind,
		Scratch:   scratch,
		AuditPath: auditPath,
		jobs:      make(map[string]*job),
	}
}

func validateBind(bind string) error {
	host, port, err := net.SplitHostPort(bind)
	if err != nil {
		return fmt.Errorf("bind must be host:port: %w", err)
	}
	if _, bad := errOfflinePorts[port]; bad {
		return fmt.Errorf("refusing ingest/client port :%s", port)
	}
	ip := net.ParseIP(host)
	if ip != nil && ip.IsLoopback() {
		return nil
	}
	// Compose publishes 127.0.0.1:18765 on the host; the process inside the
	// container must accept 0.0.0.0. Never enable this on a public NIC.
	if os.Getenv("GROK_ANALYST_CONTAINER") == "1" && (host == "0.0.0.0" || host == "::") {
		return nil
	}
	return fmt.Errorf("sidecar must bind loopback only, got %q", bind)
}

func (s *Sidecar) Listen() error {
	if err := validateBind(s.Bind); err != nil {
		return err
	}
	if s.Scratch == "" {
		s.Scratch = filepath.Join(os.TempDir(), "grok-analyst-jails")
	}
	if forbiddenMount(s.Scratch) {
		return fmt.Errorf("scratch path not allowed")
	}
	if err := os.MkdirAll(s.Scratch, 0o700); err != nil {
		return err
	}
	if s.AuditPath == "" {
		s.AuditPath = filepath.Join(s.Scratch, "audit.log")
	}
	ln, err := net.Listen("tcp", s.Bind)
	if err != nil {
		return err
	}
	s.ln = ln
	mux := http.NewServeMux()
	mux.HandleFunc("POST /jobs", s.handleCreate)
	mux.HandleFunc("GET /jobs/{id}", s.handleGet)
	mux.HandleFunc("POST /jobs/{id}/cancel", s.handleCancel)
	mux.HandleFunc("GET /health", s.handleHealth)
	s.srv = &http.Server{
		Handler:           mux,
		ReadHeaderTimeout: 5 * time.Second,
	}
	go func() {
		if err := s.srv.Serve(ln); err != nil && !errors.Is(err, http.ErrServerClosed) {
			log.Printf("[grok-analyst] serve: %v", err)
		}
	}()
	return nil
}

func (s *Sidecar) Addr() string {
	if s.ln == nil {
		return s.Bind
	}
	return s.ln.Addr().String()
}

func (s *Sidecar) Close() error {
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		return nil
	}
	s.closed = true
	var running []*job
	for _, j := range s.jobs {
		running = append(running, j)
	}
	s.mu.Unlock()

	for _, j := range running {
		s.finish(j, "cancelled", "sidecar_stop", "sidecar shutting down")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	if s.srv != nil {
		_ = s.srv.Shutdown(ctx)
	}
	if s.ln != nil {
		_ = s.ln.Close()
	}
	s.reapJobContainers()
	s.reapAllJails()
	return nil
}

func (s *Sidecar) handleHealth(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{"ok": true, "bind": s.Addr()})
}

func (s *Sidecar) handleCreate(w http.ResponseWriter, r *http.Request) {
	var req JobRequest
	body, err := io.ReadAll(io.LimitReader(r.Body, 1<<20))
	if err != nil {
		http.Error(w, "bad body", http.StatusBadRequest)
		return
	}
	if err := json.Unmarshal(body, &req); err != nil {
		http.Error(w, "invalid json", http.StatusBadRequest)
		return
	}
	if req.JobID == "" || req.ClientID == "" {
		http.Error(w, "jobId and clientId required", http.StatusBadRequest)
		return
	}
	if !safeID(req.JobID) || !safeID(req.ClientID) {
		http.Error(w, "invalid id", http.StatusBadRequest)
		return
	}
	if req.Recipe == "" {
		req.Recipe = "summarize_window"
	}
	if req.TimeoutS <= 0 {
		req.TimeoutS = DefaultTimeoutS
	}
	if forbiddenBundle(req.BundlePath) {
		http.Error(w, "bundle path not allowed", http.StatusBadRequest)
		return
	}

	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		http.Error(w, "sidecar stopping", http.StatusServiceUnavailable)
		return
	}
	key := jobKey(req.ClientID, req.JobID)
	if _, exists := s.jobs[key]; exists {
		s.mu.Unlock()
		http.Error(w, "duplicate jobId", http.StatusConflict)
		return
	}
	var inflight, clientInflight int
	for _, j := range s.jobs {
		if !terminal(j.status) {
			inflight++
			if j.req.ClientID == req.ClientID {
				clientInflight++
			}
		}
	}
	if inflight >= MaxGlobalJobs || clientInflight >= MaxClientJobs {
		s.mu.Unlock()
		http.Error(w, "inflight limit", http.StatusTooManyRequests)
		return
	}
	j := &job{
		req:     req,
		status:  "queued",
		started: time.Now(),
		done:    make(chan struct{}),
	}
	s.jobs[key] = j
	s.mu.Unlock()

	if err := s.startJob(j); err != nil {
		s.finish(j, "failed", "container", err.Error())
		writeJSON(w, http.StatusInternalServerError, s.view(j))
		return
	}
	writeJSON(w, http.StatusAccepted, s.view(j))
}

func (s *Sidecar) handleGet(w http.ResponseWriter, r *http.Request) {
	j := s.ownedJob(w, r)
	if j == nil {
		return
	}
	writeJSON(w, http.StatusOK, s.view(j))
}

func (s *Sidecar) handleCancel(w http.ResponseWriter, r *http.Request) {
	j := s.ownedJob(w, r)
	if j == nil {
		return
	}
	s.finish(j, "cancelled", "cancelled", "client cancel")
	writeJSON(w, http.StatusOK, s.view(j))
}

// ownedJob requires the owning clientId (X-Client-Id, query, or JSON body) and
// returns the job only when that client owns it. Mismatch and unknown ids are
// both 404 so callers cannot probe another client's jobId.
func (s *Sidecar) ownedJob(w http.ResponseWriter, r *http.Request) *job {
	id := r.PathValue("id")
	clientID := requestClientID(r)
	if clientID == "" {
		http.Error(w, "clientId required", http.StatusBadRequest)
		return nil
	}
	if !safeID(clientID) || !safeID(id) {
		http.Error(w, "not found", http.StatusNotFound)
		return nil
	}
	s.mu.Lock()
	j := s.jobs[jobKey(clientID, id)]
	s.mu.Unlock()
	if j == nil {
		http.Error(w, "not found", http.StatusNotFound)
		return nil
	}
	return j
}

func (s *Sidecar) startJob(j *job) error {
	jail := filepath.Join(s.Scratch, jailSegment(j.req.ClientID), jailSegment(j.req.JobID))
	if err := rejectMount(jail); err != nil {
		return err
	}
	if err := os.MkdirAll(jail, 0o700); err != nil {
		return err
	}
	// Container uid 65534 must write result.json. Host ACLs/path modes are
	// not the security boundary — the mount list and network mode are.
	_ = os.Chmod(jail, 0o777)

	reqRaw, err := json.Marshal(j.req)
	if err != nil {
		_ = os.RemoveAll(jail)
		return err
	}
	if err := os.WriteFile(filepath.Join(jail, "request.json"), reqRaw, 0o644); err != nil {
		_ = os.RemoveAll(jail)
		return err
	}
	marker, _ := json.Marshal(map[string]string{
		"jobId":    j.req.JobID,
		"clientId": j.req.ClientID,
		"recipe":   j.req.Recipe,
	})
	if err := os.WriteFile(filepath.Join(jail, "jail.json"), marker, 0o600); err != nil {
		_ = os.RemoveAll(jail)
		return err
	}
	nBundle, err := installBundle(jail, j.req.BundlePath)
	if err != nil {
		_ = os.RemoveAll(jail)
		return err
	}

	timeout := time.Duration(j.req.TimeoutS) * time.Second
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	name := ContainerName(j.req.ClientID, j.req.JobID)
	s.mu.Lock()
	j.jail = jail
	j.container = name
	j.status = "running"
	j.cancel = cancel
	j.bytesIn = int64(len(reqRaw)) + nBundle
	s.mu.Unlock()

	go s.runJobContainer(ctx, j)
	return nil
}

func (s *Sidecar) runJobContainer(ctx context.Context, j *job) {
	defer func() {
		s.mu.Lock()
		if j.cancel != nil {
			j.cancel()
			j.cancel = nil
		}
		s.mu.Unlock()
	}()

	err := s.runContainer(ctx, j)
	s.mu.Lock()
	jail := j.jail
	done := terminal(j.status)
	s.mu.Unlock()
	if done {
		return
	}
	if ctx.Err() != nil {
		if errors.Is(ctx.Err(), context.DeadlineExceeded) {
			s.finish(j, "failed", "timeout", "job wall timeout")
			return
		}
		s.finish(j, "cancelled", "cancelled", "job cancelled")
		return
	}

	res, readErr := readJobResult(jail)
	if readErr != nil {
		msg := readErr.Error()
		if err != nil {
			msg = strings.TrimSpace(err.Error() + ": " + msg)
		}
		code := "container"
		low := strings.ToLower(msg)
		if strings.Contains(low, "oom") || strings.Contains(low, "exit status 137") {
			code = "oom"
		}
		s.finish(j, "failed", code, msg)
		return
	}
	if res.Status == "succeeded" || res.Status == "" {
		summary := res.Summary
		if len(summary) > MaxSummaryBytes {
			summary = summary[:MaxSummaryBytes]
		}
		s.finish(j, "succeeded", "", summary)
		return
	}
	s.finish(j, res.Status, res.Code, res.Message)
}

func (s *Sidecar) finish(j *job, status, code, message string) {
	s.mu.Lock()
	if terminal(j.status) || j.closing {
		s.mu.Unlock()
		return
	}
	j.closing = true
	if j.cancel != nil {
		j.cancel()
		j.cancel = nil
	}
	jail := j.jail
	name := j.container
	j.jail = ""
	j.container = ""
	s.mu.Unlock()

	s.killContainer(name)
	destroyJail(jail)

	s.mu.Lock()
	if terminal(j.status) {
		s.mu.Unlock()
		return
	}
	j.status = status
	j.code = code
	if status == "succeeded" {
		j.summary = message
		j.message = ""
		j.bytesOut = int64(len(message))
	} else {
		j.message = message
	}
	j.elapsedMs = time.Since(j.started).Milliseconds()
	select {
	case <-j.done:
	default:
		close(j.done)
	}
	s.mu.Unlock()
	s.audit(j)
}

func (s *Sidecar) view(j *job) JobView {
	s.mu.Lock()
	defer s.mu.Unlock()
	v := JobView{
		JobID:          j.req.JobID,
		ClientID:       j.req.ClientID,
		Status:         j.status,
		Recipe:         j.req.Recipe,
		SessionID:      j.req.SessionID,
		Summary:        j.summary,
		Code:           j.code,
		Message:        j.message,
		JailAlive:      j.jail != "" && dirExists(j.jail),
		Container:      j.container,
		ContainerAlive: j.container != "" && !terminal(j.status),
		ElapsedMs:      j.elapsedMs,
		BytesIn:        j.bytesIn,
		BytesOut:       j.bytesOut,
	}
	if v.JailAlive {
		v.JailPath = j.jail
	}
	if !terminal(j.status) {
		v.ElapsedMs = time.Since(j.started).Milliseconds()
	}
	return v
}

func (s *Sidecar) audit(j *job) {
	if s.AuditPath == "" {
		return
	}
	line := fmt.Sprintf("%s,%s,%s,%s,%d,%d,%d,%s,%s,%d,%d,%d\n",
		time.Now().UTC().Format(time.RFC3339),
		j.req.ClientID, j.req.JobID, j.req.SessionID,
		j.req.T0, j.req.T1, j.req.SignalCount, j.req.Recipe,
		j.status, j.elapsedMs, j.bytesIn, j.bytesOut)
	if strings.Contains(strings.ToLower(line), "key=") || strings.Contains(strings.ToLower(line), "secret") {
		return
	}
	f, err := os.OpenFile(s.AuditPath, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o600)
	if err != nil {
		return
	}
	_, _ = f.WriteString(line)
	_ = f.Close()
}

func (s *Sidecar) reapAllJails() {
	if s.Scratch == "" {
		return
	}
	entries, err := os.ReadDir(s.Scratch)
	if err != nil {
		return
	}
	for _, e := range entries {
		if !e.IsDir() {
			continue
		}
		_ = os.RemoveAll(filepath.Join(s.Scratch, e.Name()))
	}
}

func destroyJail(jail string) {
	if jail == "" {
		return
	}
	_ = filepath.Walk(jail, func(path string, _ os.FileInfo, err error) error {
		if err == nil {
			_ = os.Chmod(path, 0o700)
		}
		return nil
	})
	for i := 0; i < 40; i++ {
		if err := os.RemoveAll(jail); err == nil {
			if _, stErr := os.Stat(jail); os.IsNotExist(stErr) {
				break
			}
		}
		time.Sleep(100 * time.Millisecond)
	}
	parent := filepath.Dir(jail)
	if entries, err := os.ReadDir(parent); err == nil && len(entries) == 0 {
		_ = os.Remove(parent)
	}
}

type jobResult struct {
	Status  string `json:"status"`
	Summary string `json:"summary"`
	Code    string `json:"code"`
	Message string `json:"message"`
}

func readJobResult(jail string) (jobResult, error) {
	var res jobResult
	if jail == "" {
		return res, fmt.Errorf("missing job scratch")
	}
	raw, err := os.ReadFile(filepath.Join(jail, "result.json"))
	if err != nil {
		return res, err
	}
	if err := json.Unmarshal(raw, &res); err != nil {
		return res, err
	}
	return res, nil
}

func installBundle(jail, src string) (int64, error) {
	if strings.TrimSpace(src) == "" {
		return 0, nil
	}
	if forbiddenBundle(src) || forbiddenMount(src) {
		return 0, fmt.Errorf("bundle path not allowed")
	}
	info, err := os.Stat(src)
	if err != nil {
		return 0, err
	}
	if info.IsDir() {
		return 0, fmt.Errorf("bundle path is a directory")
	}
	if info.Size() > MaxBundleBytes {
		return 0, fmt.Errorf("bundle exceeds 32 MB cap")
	}
	in, err := os.Open(src)
	if err != nil {
		return 0, err
	}
	defer in.Close()
	dst := filepath.Join(jail, "bundle.json")
	out, err := os.OpenFile(dst, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o444)
	if err != nil {
		return 0, err
	}
	defer out.Close()
	n, err := io.Copy(out, io.LimitReader(in, MaxBundleBytes+1))
	if err != nil {
		return 0, err
	}
	if n > MaxBundleBytes {
		_ = os.Remove(dst)
		return 0, fmt.Errorf("bundle exceeds 32 MB cap")
	}
	return n, nil
}

func terminal(status string) bool {
	return status == "succeeded" || status == "failed" || status == "cancelled"
}

func jobKey(clientID, jobID string) string {
	return clientID + "\x1f" + jobID
}

func jailSegment(id string) string {
	return strings.Map(func(r rune) rune {
		switch r {
		case ':', '/', '\\', '*', '?', '"', '<', '>', '|':
			return '_'
		default:
			return r
		}
	}, id)
}

func requestClientID(r *http.Request) string {
	if v := strings.TrimSpace(r.Header.Get("X-Client-Id")); v != "" {
		return v
	}
	if v := strings.TrimSpace(r.URL.Query().Get("clientId")); v != "" {
		return v
	}
	if r.Body == nil || (r.Method != http.MethodPost && r.Method != http.MethodPut && r.Method != http.MethodPatch) {
		return ""
	}
	raw, err := io.ReadAll(io.LimitReader(r.Body, 1<<20))
	if err != nil {
		return ""
	}
	r.Body = io.NopCloser(bytes.NewReader(raw))
	var payload struct {
		ClientID string `json:"clientId"`
	}
	if json.Unmarshal(raw, &payload) != nil {
		return ""
	}
	return strings.TrimSpace(payload.ClientID)
}

func safeID(id string) bool {
	if id == "" || len(id) > 96 {
		return false
	}
	for _, c := range id {
		// Colon is allowed: the backend sends "<numericId>:<label>".
		if (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
			c == '_' || c == '-' || c == '.' || c == ':' {
			continue
		}
		return false
	}
	return true
}

func forbiddenBundle(path string) bool {
	if path == "" {
		return false
	}
	norm := strings.ToLower(filepath.ToSlash(path))
	if strings.Contains(norm, "data/tsdb") || strings.Contains(norm, "data/video") {
		return true
	}
	if strings.Contains(norm, "docker.sock") {
		return true
	}
	info, err := os.Stat(path)
	if err != nil {
		return false
	}
	return info.Size() > MaxBundleBytes
}

func dirExists(p string) bool {
	st, err := os.Stat(p)
	return err == nil && st.IsDir()
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}
