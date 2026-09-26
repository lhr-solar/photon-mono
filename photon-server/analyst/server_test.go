package analyst

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func writeBundle(t *testing.T, dir, name string, body string) string {
	t.Helper()
	if err := os.MkdirAll(dir, 0o700); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(dir, name)
	if err := os.WriteFile(path, []byte(body), 0o600); err != nil {
		t.Fatal(err)
	}
	return path
}

func startServer(t *testing.T, adapter Adapter) *Server {
	t.Helper()
	scratch := t.TempDir()
	srv, err := NewServer(Config{
		Bind:    "127.0.0.1:0",
		Scratch: scratch,
		Adapter: adapter,
	})
	if err != nil {
		t.Fatal(err)
	}
	if err := srv.Start(); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = srv.Close() })
	return srv
}

func postJob(t *testing.T, base string, req JobRequest) (int, Result) {
	t.Helper()
	raw, _ := json.Marshal(req)
	res, err := http.Post("http://"+base+"/jobs", "application/json", bytes.NewReader(raw))
	if err != nil {
		t.Fatal(err)
	}
	defer res.Body.Close()
	body, _ := io.ReadAll(res.Body)
	var out Result
	if err := json.Unmarshal(body, &out); err != nil {
		t.Fatalf("decode %s: %v", body, err)
	}
	return res.StatusCode, out
}

func getJob(t *testing.T, base, id string) (int, Result) {
	t.Helper()
	res, err := http.Get("http://" + base + "/jobs/" + id)
	if err != nil {
		t.Fatal(err)
	}
	defer res.Body.Close()
	body, _ := io.ReadAll(res.Body)
	var out Result
	if err := json.Unmarshal(body, &out); err != nil {
		t.Fatalf("decode %s: %v", body, err)
	}
	return res.StatusCode, out
}

func waitTerminal(t *testing.T, base, id string) Result {
	t.Helper()
	deadline := time.Now().Add(3 * time.Second)
	var last Result
	for time.Now().Before(deadline) {
		_, last = getJob(t, base, id)
		if last.Status == "succeeded" || last.Status == "failed" || last.Status == "cancelled" {
			return last
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatalf("job %s did not finish: %+v", id, last)
	return last
}

func TestRefusePublicBind(t *testing.T) {
	if _, err := ParseBind("0.0.0.0:18765"); err == nil {
		t.Fatal("expected 0.0.0.0 bind to fail")
	}
	if _, err := ParseBind("192.168.1.10:18765"); err == nil {
		t.Fatal("expected LAN bind to fail")
	}
	got, err := ParseBind("127.0.0.1:18765")
	if err != nil {
		t.Fatal(err)
	}
	if got != "127.0.0.1:18765" {
		t.Fatalf("got %s", got)
	}
}

func TestScratchNotTSDB(t *testing.T) {
	_, err := NewServer(Config{Bind: "127.0.0.1:0", Scratch: filepath.Join(t.TempDir(), "tsdb")})
	if err == nil {
		t.Fatal("expected tsdb scratch to fail")
	}
}

func TestSummarizeWindowNoTSDBWrite(t *testing.T) {
	srv := startServer(t, nil)
	tsdb := filepath.Join(t.TempDir(), "tsdb")
	if err := os.MkdirAll(tsdb, 0o700); err != nil {
		t.Fatal(err)
	}
	marker := filepath.Join(tsdb, "catalog.json")
	if err := os.WriteFile(marker, []byte(`{"ok":true}`), 0o600); err != nil {
		t.Fatal(err)
	}
	before, err := os.ReadFile(marker)
	if err != nil {
		t.Fatal(err)
	}

	bundle := writeBundle(t, t.TempDir(), "bundle.json", `{
		"sessionId":"sess-1","dbcHash":"abc","t0Ms":1000,"t1Ms":2000,
		"signalIds":[1,2],"maxPoints":200
	}`)
	code, created := postJob(t, srv.Addr(), JobRequest{
		JobID: "job-sum", ClientID: "client-a", BundlePath: bundle,
		Recipe: "summarize_window", SessionID: "sess-1", T0Ms: 1000, T1Ms: 2000, SignalCount: 2,
	})
	if code != http.StatusAccepted && code != http.StatusOK {
		t.Fatalf("post status %d %+v", code, created)
	}
	got := waitTerminal(t, srv.Addr(), "job-sum")
	if got.Status != "succeeded" {
		t.Fatalf("status %s code=%s msg=%s", got.Status, got.Code, got.Message)
	}
	if !strings.Contains(got.Summary, "sess-1") {
		t.Fatalf("summary %q", got.Summary)
	}
	after, err := os.ReadFile(marker)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(before, after) {
		t.Fatal("TSDB catalog.json changed")
	}
	entries, _ := os.ReadDir(tsdb)
	if len(entries) != 1 {
		t.Fatalf("tsdb dir grew: %d entries", len(entries))
	}
}

func TestClientJailsAreIsolated(t *testing.T) {
	srv := startServer(t, nil)
	a := writeBundle(t, t.TempDir(), "a.json", `{"sessionId":"A","t0Ms":0,"t1Ms":1}`)
	b := writeBundle(t, t.TempDir(), "b.json", `{"sessionId":"B","t0Ms":0,"t1Ms":1}`)
	if _, created := postJob(t, srv.Addr(), JobRequest{JobID: "ja", ClientID: "client-a", BundlePath: a, Recipe: "summarize_window"}); created.JobID != "ja" {
		t.Fatalf("a: %+v", created)
	}
	if _, created := postJob(t, srv.Addr(), JobRequest{JobID: "jb", ClientID: "client-b", BundlePath: b, Recipe: "summarize_window"}); created.JobID != "jb" {
		t.Fatalf("b: %+v", created)
	}
	dirA := filepath.Join(srv.cfg.Scratch, "jobs", "client-a", "ja")
	dirB := filepath.Join(srv.cfg.Scratch, "jobs", "client-b", "jb")
	if dirA == dirB {
		t.Fatal("clients shared a jail dir")
	}
	waitTerminal(t, srv.Addr(), "ja")
	waitTerminal(t, srv.Addr(), "jb")
	if _, err := os.Stat(dirA); !os.IsNotExist(err) {
		t.Fatalf("jail A leftover: %v", err)
	}
	if _, err := os.Stat(dirB); !os.IsNotExist(err) {
		t.Fatalf("jail B leftover: %v", err)
	}
}

type hangAdapter struct {
	started chan struct{}
}

func (h hangAdapter) Start(ctx context.Context, job *Job, jail *Jail) Result {
	if h.started != nil {
		select {
		case h.started <- struct{}{}:
		case <-ctx.Done():
			return Result{JobID: job.ID, Status: "cancelled", Code: "analysis_cancelled", Message: "job cancelled"}
		}
	}
	<-ctx.Done()
	return Result{JobID: job.ID, Status: "cancelled", Code: "analysis_cancelled", Message: "job cancelled"}
}

func TestCancelRemovesJail(t *testing.T) {
	started := make(chan struct{})
	srv := startServer(t, hangAdapter{started: started})
	bundle := writeBundle(t, t.TempDir(), "bundle.json", `{"sessionId":"c"}`)
	_, created := postJob(t, srv.Addr(), JobRequest{
		JobID: "job-c", ClientID: "client-c", BundlePath: bundle, Recipe: "summarize_window", TimeoutS: 30,
	})
	if created.JobID != "job-c" {
		t.Fatalf("%+v", created)
	}
	select {
	case <-started:
	case <-time.After(2 * time.Second):
		t.Fatal("adapter did not start")
	}
	jail := srv.JailDir("client-c", "job-c")
	res, err := http.Post("http://"+srv.Addr()+"/jobs/job-c/cancel", "application/json", strings.NewReader("{}"))
	if err != nil {
		t.Fatal(err)
	}
	res.Body.Close()
	got := waitTerminal(t, srv.Addr(), "job-c")
	if got.Status != "cancelled" {
		t.Fatalf("status %s", got.Status)
	}
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if _, err := os.Stat(jail); os.IsNotExist(err) {
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatalf("jail leftover at %s", jail)
}

func TestRejectTSDBBundlePath(t *testing.T) {
	srv := startServer(t, nil)
	tsdb := filepath.Join(t.TempDir(), "data", "tsdb", "sess", "bundle.json")
	if err := os.MkdirAll(filepath.Dir(tsdb), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(tsdb, []byte(`{"sessionId":"nope"}`), 0o600); err != nil {
		t.Fatal(err)
	}
	code, got := postJob(t, srv.Addr(), JobRequest{
		JobID: "job-tsdb", ClientID: "client-a", BundlePath: tsdb, Recipe: "summarize_window",
	})
	if code < 400 {
		t.Fatalf("expected reject, got %d %+v", code, got)
	}
	if got.Code == "" && !strings.Contains(got.Message, "TSDB") {
		t.Fatalf("expected tsdb error, got %+v", got)
	}
}

func TestInflightLimits(t *testing.T) {
	started := make(chan struct{}, 4)
	srv := startServer(t, hangAdapter{started: started})
	bundle := writeBundle(t, t.TempDir(), "bundle.json", `{"sessionId":"lim"}`)
	_, a := postJob(t, srv.Addr(), JobRequest{JobID: "l1", ClientID: "ca", BundlePath: bundle, Recipe: "summarize_window", TimeoutS: 30})
	if a.Status == "failed" {
		t.Fatalf("first job failed: %+v", a)
	}
	code, b := postJob(t, srv.Addr(), JobRequest{JobID: "l2", ClientID: "ca", BundlePath: bundle, Recipe: "summarize_window", TimeoutS: 30})
	if code != http.StatusServiceUnavailable || b.Code != "analysis_busy" {
		t.Fatalf("same client second job: %d %+v", code, b)
	}
	_, c := postJob(t, srv.Addr(), JobRequest{JobID: "l3", ClientID: "cb", BundlePath: bundle, Recipe: "summarize_window", TimeoutS: 30})
	if c.Status == "failed" && c.Code == "analysis_busy" {
		t.Fatalf("second client should be allowed: %+v", c)
	}
	code, d := postJob(t, srv.Addr(), JobRequest{JobID: "l4", ClientID: "cc", BundlePath: bundle, Recipe: "summarize_window", TimeoutS: 30})
	if code != http.StatusServiceUnavailable || d.Code != "analysis_busy" {
		t.Fatalf("global third job: %d %+v", code, d)
	}
}

func TestAuditLineNoSecrets(t *testing.T) {
	srv := startServer(t, nil)
	bundle := writeBundle(t, t.TempDir(), "bundle.json", `{"sessionId":"aud","prompt":"password=hunter2"}`)
	postJob(t, srv.Addr(), JobRequest{
		JobID: "job-aud", ClientID: "client-aud", BundlePath: bundle,
		Recipe: "summarize_window", SessionID: "aud",
	})
	waitTerminal(t, srv.Addr(), "job-aud")
	raw, err := os.ReadFile(srv.audit.Path())
	if err != nil {
		t.Fatal(err)
	}
	line := string(raw)
	if !strings.Contains(line, "job-aud") || !strings.Contains(line, "client-aud") {
		t.Fatalf("audit missing ids: %s", line)
	}
	if strings.Contains(strings.ToLower(line), "hunter2") || strings.Contains(strings.ToLower(line), "password=") {
		t.Fatalf("secret leaked into audit: %s", line)
	}
}

func TestBundleCap(t *testing.T) {
	srv := startServer(t, nil)
	big := filepath.Join(t.TempDir(), "big.json")
	f, err := os.Create(big)
	if err != nil {
		t.Fatal(err)
	}
	if err := f.Truncate(maxBundleBytes + 8); err != nil {
		t.Fatal(err)
	}
	f.Close()
	code, got := postJob(t, srv.Addr(), JobRequest{
		JobID: "job-big", ClientID: "client-a", BundlePath: big, Recipe: "summarize_window",
	})
	if code < 400 {
		t.Fatalf("expected cap reject %d %+v", code, got)
	}
}

func TestUnknownRecipeFailsCleanly(t *testing.T) {
	srv := startServer(t, nil)
	bundle := writeBundle(t, t.TempDir(), "bundle.json", `{"sessionId":"x"}`)
	_, created := postJob(t, srv.Addr(), JobRequest{
		JobID: "job-unk", ClientID: "client-a", BundlePath: bundle, Recipe: "not_a_recipe",
	})
	got := created
	if got.Status != "failed" {
		got = waitTerminal(t, srv.Addr(), "job-unk")
	}
	if got.Status != "failed" {
		t.Fatalf("%+v", got)
	}
}

func TestCloseCancelsInflight(t *testing.T) {
	started := make(chan struct{})
	scratch := t.TempDir()
	srv, err := NewServer(Config{Bind: "127.0.0.1:0", Scratch: scratch, Adapter: hangAdapter{started: started}})
	if err != nil {
		t.Fatal(err)
	}
	if err := srv.Start(); err != nil {
		t.Fatal(err)
	}
	bundle := writeBundle(t, t.TempDir(), "bundle.json", `{"sessionId":"stop"}`)
	postJob(t, srv.Addr(), JobRequest{JobID: "job-stop", ClientID: "client-s", BundlePath: bundle, Recipe: "summarize_window", TimeoutS: 30})
	select {
	case <-started:
	case <-time.After(2 * time.Second):
		t.Fatal("did not start")
	}
	jail := srv.JailDir("client-s", "job-stop")
	if err := srv.Close(); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(jail); err == nil {
		t.Fatalf("jail leftover after close: %s", jail)
	}
}
