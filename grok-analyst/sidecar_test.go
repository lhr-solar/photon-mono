package analyst

import (
	"bytes"
	"encoding/json"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func startTestSidecar(t *testing.T) (*Sidecar, string) {
	t.Helper()
	root := t.TempDir()
	scratch := filepath.Join(root, "jails")
	audit := filepath.Join(root, "audit.log")
	s := New("127.0.0.1:0", scratch, audit)
	if err := s.Listen(); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = s.Close() })
	return s, "http://" + s.Addr()
}

func postJob(t *testing.T, base string, req JobRequest) (int, JobView) {
	t.Helper()
	body, _ := json.Marshal(req)
	resp, err := http.Post(base+"/jobs", "application/json", bytes.NewReader(body))
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	raw, _ := io.ReadAll(resp.Body)
	var view JobView
	_ = json.Unmarshal(raw, &view)
	return resp.StatusCode, view
}

func doJob(t *testing.T, method, url, clientID string, body io.Reader) (int, JobView, string) {
	t.Helper()
	req, err := http.NewRequest(method, url, body)
	if err != nil {
		t.Fatal(err)
	}
	if clientID != "" {
		req.Header.Set("X-Client-Id", clientID)
	}
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	raw, _ := io.ReadAll(resp.Body)
	var view JobView
	_ = json.Unmarshal(raw, &view)
	return resp.StatusCode, view, string(raw)
}

func getJob(t *testing.T, base, id, clientID string) (int, JobView) {
	t.Helper()
	code, view, _ := doJob(t, http.MethodGet, base+"/jobs/"+id, clientID, nil)
	return code, view
}

func cancelJob(t *testing.T, base, id, clientID string) (int, JobView) {
	t.Helper()
	code, view, _ := doJob(t, http.MethodPost, base+"/jobs/"+id+"/cancel", clientID, nil)
	return code, view
}

func TestRefuseIngestPorts(t *testing.T) {
	for _, bind := range []string{"127.0.0.1:6100", "127.0.0.1:6500", "127.0.0.1:6700", "0.0.0.0:18765"} {
		s := New(bind, t.TempDir(), "")
		if err := s.Listen(); err == nil {
			_ = s.Close()
			t.Fatalf("expected bind reject for %s", bind)
		}
	}
}

func TestSummarizeNoTsdbWrite(t *testing.T) {
	s, base := startContainerSidecar(t)
	tsdb := filepath.Join(t.TempDir(), "data", "tsdb")
	if err := os.MkdirAll(tsdb, 0o700); err != nil {
		t.Fatal(err)
	}
	marker := filepath.Join(tsdb, "catalog.json")
	if err := os.WriteFile(marker, []byte(`{"ok":true}`), 0o600); err != nil {
		t.Fatal(err)
	}
	before, _ := os.ReadFile(marker)
	info, _ := os.Stat(marker)

	code, view := postJob(t, base, JobRequest{
		JobID: "job-sum", ClientID: "client-a", Recipe: "summarize_window",
		SessionID: "sess-1", T0: 1000, T1: 2000, SignalCount: 2, HoldMs: 5,
	})
	if code != http.StatusAccepted {
		t.Fatalf("POST /jobs status %d", code)
	}
	view = waitJobStatus(t, base, "job-sum", "client-a", "succeeded", 60*time.Second)
	if view.Status != "succeeded" {
		t.Fatalf("status=%s msg=%s", view.Status, view.Message)
	}
	if view.JailAlive {
		t.Fatal("jail leftover after success")
	}
	if _, err := os.Stat(filepath.Join(s.Scratch, "client-a", "job-sum")); !os.IsNotExist(err) {
		t.Fatalf("jail dir still present: %v", err)
	}

	after, _ := os.ReadFile(marker)
	afterInfo, _ := os.Stat(marker)
	if string(after) != string(before) || afterInfo.Size() != info.Size() {
		t.Fatal("TSDB catalog changed")
	}
	entries, _ := os.ReadDir(tsdb)
	if len(entries) != 1 {
		t.Fatalf("TSDB dir grew: %d files", len(entries))
	}
}

func TestCancelDestroysJail(t *testing.T) {
	s, base := startContainerSidecar(t)
	code, view := postJob(t, base, JobRequest{
		JobID: "job-hold", ClientID: "client-a", Recipe: "freeform",
		TimeoutS: 90, HoldMs: 60000,
	})
	if code != http.StatusAccepted || view.Status == "succeeded" {
		t.Fatalf("expected running job, got %d %s", code, view.Status)
	}
	name := ContainerName("client-a", "job-hold")
	waitContainerRunning(t, name, 45*time.Second)
	jail := filepath.Join(s.Scratch, "client-a", "job-hold")
	if !dirExists(jail) {
		t.Fatal("expected jail while running")
	}

	code, view = cancelJob(t, base, "job-hold", "client-a")
	if code != http.StatusOK {
		t.Fatalf("cancel status %d", code)
	}
	_, view = getJob(t, base, "job-hold", "client-a")
	if view.Status != "cancelled" {
		t.Fatalf("status=%s", view.Status)
	}
	if view.JailAlive || dirExists(jail) {
		t.Fatal("leftover jail after cancel")
	}
	deadline := time.Now().Add(15 * time.Second)
	for time.Now().Before(deadline) && containerExists(name) {
		time.Sleep(100 * time.Millisecond)
	}
	if containerExists(name) {
		t.Fatal("leftover container after cancel")
	}
}

func TestTimeoutDestroysJail(t *testing.T) {
	s, base := startContainerSidecar(t)
	_, view := postJob(t, base, JobRequest{
		JobID: "job-to", ClientID: "client-b", Recipe: "freeform",
		TimeoutS: 45, HoldMs: 180000,
	})
	name := ContainerName("client-b", "job-to")
	waitContainerRunning(t, name, 45*time.Second)
	view = waitJobStatus(t, base, "job-to", "client-b", "failed", 70*time.Second)
	if view.Status != "failed" || view.Code != "timeout" {
		t.Fatalf("status=%s code=%s msg=%s", view.Status, view.Code, view.Message)
	}
	if dirExists(filepath.Join(s.Scratch, "client-b", "job-to")) {
		t.Fatal("leftover jail after timeout")
	}
	if containerExists(name) {
		t.Fatal("leftover container after timeout")
	}
}

func TestClientIsolation(t *testing.T) {
	s, base := startContainerSidecar(t)
	_, a := postJob(t, base, JobRequest{
		JobID: "job-a", ClientID: "alice", Recipe: "freeform", HoldMs: 45000, TimeoutS: 90,
	})
	_, b := postJob(t, base, JobRequest{
		JobID: "job-b", ClientID: "bob", Recipe: "freeform", HoldMs: 45000, TimeoutS: 90,
	})
	waitContainerRunning(t, ContainerName("alice", "job-a"), 45*time.Second)
	waitContainerRunning(t, ContainerName("bob", "job-b"), 45*time.Second)
	_, a = getJob(t, base, "job-a", "alice")
	_, b = getJob(t, base, "job-b", "bob")
	if !a.JailAlive || !b.JailAlive {
		t.Fatal("expected both jails alive")
	}
	if !a.ContainerAlive || !b.ContainerAlive {
		t.Fatal("expected both containers alive")
	}
	if a.Container == b.Container {
		t.Fatal("clients shared a container")
	}
	aliceJail := filepath.Join(s.Scratch, "alice", "job-a")
	bobJail := filepath.Join(s.Scratch, "bob", "job-b")
	if _, err := os.Stat(filepath.Join(aliceJail, "jail.json")); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(bobJail, "jail.json")); err != nil {
		t.Fatal(err)
	}
	// Alice's tree must not contain Bob's job.
	if _, err := os.Stat(filepath.Join(s.Scratch, "alice", "job-b")); !os.IsNotExist(err) {
		t.Fatal("client A can see client B jail")
	}
	code, got := getJob(t, base, "job-a", "alice")
	if code != http.StatusOK || got.ClientID != "alice" || strings.Contains(got.JailPath, "bob") {
		t.Fatalf("leaked B into A view: %d %+v", code, got)
	}
}

func TestAuditHasNoSecrets(t *testing.T) {
	s, base := startContainerSidecar(t)
	_, _ = postJob(t, base, JobRequest{
		JobID: "job-aud", ClientID: "client-a", Recipe: "summarize_window",
		SessionID: "sess", T0: 1, T1: 2, SignalCount: 1, HoldMs: 5,
	})
	_ = waitJobStatus(t, base, "job-aud", "client-a", "succeeded", 60*time.Second)
	raw, err := os.ReadFile(s.AuditPath)
	if err != nil {
		t.Fatal(err)
	}
	line := string(raw)
	if !strings.Contains(line, "job-aud") || !strings.Contains(line, "client-a") {
		t.Fatalf("audit missing fields: %s", line)
	}
	low := strings.ToLower(line)
	for _, bad := range []string{"api_key", "secret", "password", "bearer", "xai-"} {
		if strings.Contains(low, bad) {
			t.Fatalf("audit leaked %s: %s", bad, line)
		}
	}
}

func TestCloseReapsJails(t *testing.T) {
	requireDocker(t)
	ensureJobImage(t)
	root := t.TempDir()
	scratch := filepath.Join(root, "jails")
	s := New("127.0.0.1:0", scratch, filepath.Join(root, "audit.log"))
	if err := s.Listen(); err != nil {
		t.Fatal(err)
	}
	base := "http://" + s.Addr()
	_, _ = postJob(t, base, JobRequest{
		JobID: "job-stop", ClientID: "client-a", Recipe: "freeform", HoldMs: 60000, TimeoutS: 90,
	})
	name := ContainerName("client-a", "job-stop")
	waitContainerRunning(t, name, 45*time.Second)
	if !dirExists(filepath.Join(scratch, "client-a", "job-stop")) {
		t.Fatal("expected jail")
	}
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	if dirExists(filepath.Join(scratch, "client-a", "job-stop")) {
		t.Fatal("leftover jail after Close")
	}
	if containerExists(name) {
		t.Fatal("leftover container after Close")
	}
}

func TestRejectTsdbBundlePath(t *testing.T) {
	_, base := startTestSidecar(t)
	code, _ := postJob(t, base, JobRequest{
		JobID: "job-bad", ClientID: "client-a",
		BundlePath: `F:\lhr\data\tsdb\sess`, Recipe: "summarize_window",
	})
	if code != http.StatusBadRequest {
		t.Fatalf("expected 400, got %d", code)
	}
}

func TestCrossClientCannotStealJob(t *testing.T) {
	_, base := startTestSidecar(t)
	code, view := postJob(t, base, JobRequest{
		JobID: "shared-id", ClientID: "client-a", Recipe: "summarize_window",
		SessionID: "secret-session-a", T0: 1, T1: 2, SignalCount: 1, HoldMs: 8000, TimeoutS: 30,
	})
	if code != http.StatusAccepted && code != http.StatusInternalServerError {
		t.Fatalf("POST /jobs status %d", code)
	}
	if view.SessionID != "secret-session-a" {
		t.Fatalf("owner POST missing session: %+v", view)
	}
	ownerCode, _, ownerRaw := doJob(t, http.MethodGet, base+"/jobs/shared-id", "client-a", nil)
	if ownerCode != http.StatusOK || !strings.Contains(ownerRaw, "secret-session-a") {
		t.Fatalf("owner GET missing secret: %d %s", ownerCode, ownerRaw)
	}

	stolenGet, stolenView, stolenRaw := doJob(t, http.MethodGet, base+"/jobs/shared-id", "client-b", nil)
	if stolenGet != http.StatusNotFound {
		t.Fatalf("client-b GET status %d body %s", stolenGet, stolenRaw)
	}
	if stolenView.Summary != "" || strings.Contains(stolenRaw, "secret-session-a") {
		t.Fatalf("client-b saw owner secret: %s", stolenRaw)
	}

	anonGet, _, anonRaw := doJob(t, http.MethodGet, base+"/jobs/shared-id", "", nil)
	if anonGet != http.StatusBadRequest {
		t.Fatalf("anonymous GET status %d body %s", anonGet, anonRaw)
	}
	if strings.Contains(anonRaw, "secret-session-a") {
		t.Fatalf("anonymous GET leaked secret: %s", anonRaw)
	}

	stolenCancel, _, cancelRaw := doJob(t, http.MethodPost, base+"/jobs/shared-id/cancel", "client-b", nil)
	if stolenCancel != http.StatusNotFound {
		t.Fatalf("client-b cancel status %d body %s", stolenCancel, cancelRaw)
	}
	if strings.Contains(cancelRaw, "secret-session-a") {
		t.Fatalf("client-b cancel leaked secret: %s", cancelRaw)
	}

	bodyOnly, _ := json.Marshal(map[string]string{"clientId": "client-b"})
	bodyCancel, _, bodyRaw := doJob(t, http.MethodPost, base+"/jobs/shared-id/cancel", "", bytes.NewReader(bodyOnly))
	if bodyCancel != http.StatusNotFound {
		t.Fatalf("client-b body cancel status %d body %s", bodyCancel, bodyRaw)
	}
	if strings.Contains(bodyRaw, "secret-session-a") {
		t.Fatalf("client-b body cancel leaked secret: %s", bodyRaw)
	}

	laterCode, later := getJob(t, base, "shared-id", "client-a")
	if laterCode != http.StatusOK {
		t.Fatalf("owner lost job after steal attempts: %d %+v", laterCode, later)
	}
	queryCode, _, queryRaw := doJob(t, http.MethodGet, base+"/jobs/shared-id?clientId=client-a", "", nil)
	if queryCode != http.StatusOK || !strings.Contains(queryRaw, "secret-session-a") {
		t.Fatalf("owner query GET failed: %d %s", queryCode, queryRaw)
	}

	dupBody, _ := json.Marshal(JobRequest{
		JobID: "shared-id", ClientID: "client-b", Recipe: "summarize_window",
		SessionID: "session-b", HoldMs: 5,
	})
	dup, dupView, dupRaw := doJob(t, http.MethodPost, base+"/jobs", "", bytes.NewReader(dupBody))
	if dup == http.StatusOK && dupView.ClientID == "client-a" {
		t.Fatalf("client-b duplicate POST adopted owner job: %s", dupRaw)
	}
	if dup != http.StatusAccepted && dup != http.StatusTooManyRequests && dup != http.StatusInternalServerError {
		t.Fatalf("client-b duplicate POST should not adopt owner job, got %d %s", dup, dupRaw)
	}
	if strings.Contains(dupRaw, "secret-session-a") || dupView.ClientID == "client-a" ||
		dupView.SessionID == "secret-session-a" {
		t.Fatalf("client-b adopted client-a job: %s", dupRaw)
	}
}

func TestCancelClientIDInBody(t *testing.T) {
	_, base := startTestSidecar(t)
	code, view := postJob(t, base, JobRequest{
		JobID: "job-body", ClientID: "client-a", Recipe: "freeform",
		TimeoutS: 30, HoldMs: 8000,
	})
	if code != http.StatusAccepted && code != http.StatusInternalServerError {
		t.Fatalf("POST /jobs status %d", code)
	}
	payload, _ := json.Marshal(map[string]string{"clientId": "client-a"})
	code, view, raw := doJob(t, http.MethodPost, base+"/jobs/job-body/cancel", "", bytes.NewReader(payload))
	if code != http.StatusOK {
		t.Fatalf("body cancel status %d %s", code, raw)
	}
	if view.ClientID != "client-a" {
		t.Fatalf("body cancel returned other client: %+v", view)
	}
	stolen, _, stolenRaw := doJob(t, http.MethodPost, base+"/jobs/job-body/cancel", "",
		bytes.NewReader([]byte(`{"clientId":"client-b"}`)))
	if stolen != http.StatusNotFound {
		t.Fatalf("client-b body cancel status %d %s", stolen, stolenRaw)
	}
}
