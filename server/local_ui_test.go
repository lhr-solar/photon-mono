package main

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestUiPasswordGate(t *testing.T) {
	ok := http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusNoContent)
	})
	h := uiPasswordGate("laptop", ok)

	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/photon.html", nil))
	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("no auth: %d", rec.Code)
	}

	rec = httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodGet, "/photon.html", nil)
	req.SetBasicAuth("photon", "wrong")
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("wrong password: %d", rec.Code)
	}

	rec = httptest.NewRecorder()
	req = httptest.NewRequest(http.MethodGet, "/photon.html", nil)
	req.SetBasicAuth("photon", "laptop")
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusNoContent {
		t.Fatalf("good password: %d", rec.Code)
	}
	if rec.Result().Cookies()[0].Name != uiCookieName {
		t.Fatal("expected session cookie")
	}

	rec = httptest.NewRecorder()
	req = httptest.NewRequest(http.MethodGet, "/ws", nil)
	req.AddCookie(&http.Cookie{Name: uiCookieName, Value: uiCookieValue("laptop")})
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusNoContent {
		t.Fatalf("cookie: %d", rec.Code)
	}

	rec = httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/photon.wasm", nil))
	if rec.Code != http.StatusNoContent {
		t.Fatalf("wasm should load without auth: %d", rec.Code)
	}
	rec = httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/photon.js", nil))
	if rec.Code != http.StatusNoContent {
		t.Fatalf("js should load without auth: %d", rec.Code)
	}
	rec = httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/photon.html", nil))
	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("html still needs password: %d", rec.Code)
	}

	crash := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/crash" && r.Method == http.MethodPost {
			w.WriteHeader(http.StatusNoContent)
			return
		}
		ok(w, r)
	})
	gated := uiPasswordGate("laptop", crash)
	rec = httptest.NewRecorder()
	gated.ServeHTTP(rec, httptest.NewRequest(http.MethodPost, "/crash", strings.NewReader(`{"kind":"abort"}`)))
	if rec.Code != http.StatusNoContent {
		t.Fatalf("crash POST should not need password: %d", rec.Code)
	}
	rec = httptest.NewRecorder()
	gated.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/crash", nil))
	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("crash GET still needs password: %d", rec.Code)
	}
}

func TestUiCrashHandler(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "photon-ui-crash.log")
	h := uiCrashHandler(path)

	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodPost, "/crash", bytes.NewReader([]byte(`{"kind":"abort","message":"OOM"}`))))
	if rec.Code != http.StatusNoContent {
		t.Fatalf("post: %d", rec.Code)
	}
	got, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Contains(got, []byte(`"kind":"abort"`)) {
		t.Fatalf("log %s", got)
	}

	rec = httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodPost, "/crash", bytes.NewReader([]byte(`not json`))))
	if rec.Code != http.StatusNoContent {
		t.Fatalf("raw post: %d", rec.Code)
	}
	got, _ = os.ReadFile(path)
	if !bytes.Contains(got, []byte(`"raw":"not json"`)) {
		t.Fatalf("raw wrap %s", got)
	}

	rec = httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodGet, "/crash", nil)
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("get: %d", rec.Code)
	}
	if !strings.Contains(rec.Body.String(), "OOM") {
		t.Fatalf("get body %s", rec.Body.String())
	}

	rec = httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodPut, "/crash", nil))
	if rec.Code != http.StatusMethodNotAllowed {
		t.Fatalf("put: %d", rec.Code)
	}

	var payload map[string]string
	if err := json.Unmarshal([]byte(`{"kind":"abort","message":"OOM"}`), &payload); err != nil {
		t.Fatal(err)
	}
	if payload["kind"] != "abort" {
		t.Fatal(payload)
	}
}

func TestUiFileHandlerRedirectsRoot(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "photon.html"), []byte("ok"), 0o644); err != nil {
		t.Fatal(err)
	}
	h := uiFileHandler(dir)
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/", nil))
	if rec.Code != http.StatusFound {
		t.Fatalf("root: %d", rec.Code)
	}
	if loc := rec.Header().Get("Location"); loc != "/photon.html" {
		t.Fatalf("location %q", loc)
	}
}

func TestUiFileHandlerNoStore(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "photon.wasm"), []byte("wasm"), 0o644); err != nil {
		t.Fatal(err)
	}
	h := uiFileHandler(dir)
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/photon.wasm", nil))
	if rec.Header().Get("Cache-Control") != "no-store" {
		t.Fatalf("cache-control %q", rec.Header().Get("Cache-Control"))
	}
	if rec.Header().Get("Content-Type") != "application/wasm" {
		t.Fatalf("content-type %q", rec.Header().Get("Content-Type"))
	}
}

func TestUiExportProxyKeepsQuery(t *testing.T) {
	req := httptest.NewRequest(http.MethodGet, "/export.csv?session=20260907-021613-00", nil)
	if req.URL.Path != "/export.csv" {
		t.Fatalf("path %s", req.URL.Path)
	}
	if req.URL.Query().Get("session") != "20260907-021613-00" {
		t.Fatalf("query %s", req.URL.RawQuery)
	}
	if uiExportProxy() == nil || uiWsProxy() == nil {
		t.Fatal("proxies")
	}
}

func TestResolveUiOff(t *testing.T) {
	if got := resolveUiBackend(&Config{Ui: "off"}); got != "" {
		t.Fatalf("off should skip local UI, got %q", got)
	}
	if got := resolveUiBackend(&Config{Ui: "-"}); got != "" {
		t.Fatalf("- should skip local UI, got %q", got)
	}
}

func TestResolveUiExplicit(t *testing.T) {
	const path = `C:\tools\photon-backend.exe`
	if got := resolveUiBackend(&Config{Ui: path}); got != path {
		t.Fatalf("explicit -ui: got %q", got)
	}
}

func TestLocalBackendArgs(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "catalog.json"), []byte(`{"days":[]}`), 0o644); err != nil {
		t.Fatal(err)
	}
	t.Setenv("PHOTON_ON_TRACK", dir)

	cfg := &Config{
		ClientPort:      6500,
		VideoClientPort: 6700,
		VideoUdpPort:    6800,
		DataDir:         `F:\lhr\data`,
	}
	args := localBackendArgs(cfg)
	joined := strings.Join(args, " ")
	for _, want := range []string{
		"--no-live-tail",
		"--source daq_tcp",
		"--daq-host 127.0.0.1",
		"--daq-port 6500",
		"--video-host 127.0.0.1",
		"--video-port 6700",
		"--video-udp-port 6800",
		"--tsdb-dir",
		"--ws-port 8765",
		"--log-path " + dir,
	} {
		if !strings.Contains(joined, want) {
			t.Errorf("args missing %q: %s", want, joined)
		}
	}
	if strings.Contains(joined, "--daq-host 0.0.0.0") {
		t.Fatal("local UI must use loopback fan-out, not the listen host")
	}
	if strings.Contains(joined, "--source log_replay") {
		t.Fatal("empty catalog must stay on live ingest")
	}
}

func TestLocalBackendArgsReplayDay(t *testing.T) {
	root := t.TempDir()
	day := filepath.Join(root, "days", "2026-07-19")
	if err := os.MkdirAll(day, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, "catalog.json"), []byte(`{"days":["2026-07-19"]}`), 0o644); err != nil {
		t.Fatal(err)
	}
	payload := make([]byte, 1<<20+1)
	if err := os.WriteFile(filepath.Join(day, "run.log"), payload, 0o644); err != nil {
		t.Fatal(err)
	}
	t.Setenv("PHOTON_ON_TRACK", root)

	args := localBackendArgs(&Config{ClientPort: 6500, VideoClientPort: 6700, DataDir: `F:\lhr\data`})
	joined := strings.Join(args, " ")
	if strings.Contains(joined, "--source log_replay") {
		t.Fatalf("replay day must not replace live ingest: %s", joined)
	}
	if !strings.Contains(joined, "--source daq_tcp") {
		t.Fatalf("expected live source: %s", joined)
	}
	if !strings.Contains(joined, "--log-path "+root) {
		t.Fatalf("expected on-track catalog path: %s", joined)
	}
}