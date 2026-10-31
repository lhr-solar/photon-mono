package main

import (
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestMain(m *testing.M) {
	if len(os.Args) >= 2 && os.Args[1] == "--csv" {
		os.Stdout.WriteString("t_ms,message,signal,unit,value\n10,BPS_Status,BPS_Fault,,0\n")
		os.Exit(0)
	}
	if len(os.Args) >= 2 && os.Args[1] == "--serve" {
		serveHistoryTestDouble()
		os.Exit(0)
	}
	os.Exit(m.Run())
}

func TestParseCsvExportQuery(t *testing.T) {
	q, err := parseCsvExportQuery(url.Values{
		"session": {"20260712-215252-00"},
		"level":   {"1"},
		"sig":     {"BPS_Status.BPS_Fault", "Brake_Pressure_1.Brake_Pressure"},
	})
	if err != nil {
		t.Fatal(err)
	}
	if q.Session != "20260712-215252-00" || q.Level != 1 || q.All || len(q.Sigs) != 2 {
		t.Fatalf("%+v", q)
	}
	q, err = parseCsvExportQuery(url.Values{"session": {"20260712-215252-00"}, "signals": {"all"}})
	if err != nil || !q.All || q.Level != 2 {
		t.Fatalf("%+v %v", q, err)
	}
	if _, err = parseCsvExportQuery(url.Values{}); err == nil {
		t.Fatal("missing session")
	}
	if _, err = parseCsvExportQuery(url.Values{"session": {"../etc"}}); err == nil {
		t.Fatal("bad session")
	}
}

func TestExportCSVUnknownSession(t *testing.T) {
	b := &browserService{cfg: &Config{DataDir: t.TempDir()}}
	req := httptest.NewRequest(http.MethodGet, "/export.csv?session=20260712-215252-00&level=2&signals=all", nil)
	rr := httptest.NewRecorder()
	b.exportCSV(rr, req)
	if rr.Code != http.StatusNotFound {
		t.Fatalf("status %d body %s", rr.Code, rr.Body.String())
	}
}

func TestExportCSVStreamsTsdb(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "tsdb", "20260723-212137-00")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "session.json"), []byte(`{"samples":1}`), 0o644); err != nil {
		t.Fatal(err)
	}
	catalog := `{"sessions":[{"id":"20260723-212137-00","dbc_sha":"abc","min_ms":10,"max_ms":20,"path":"` + filepath.ToSlash(dir) + `"}]}`
	if err := os.WriteFile(filepath.Join(root, "tsdb", "catalog.json"), []byte(catalog), 0o644); err != nil {
		t.Fatal(err)
	}
	exportToolOverride = os.Args[0]
	t.Cleanup(func() { exportToolOverride = "" })

	b := &browserService{cfg: &Config{DataDir: root}}
	req := httptest.NewRequest(http.MethodGet, "/export.csv?session=20260723-212137-00&level=2&sig=BPS_Status.BPS_Fault", nil)
	rr := httptest.NewRecorder()
	b.exportCSV(rr, req)
	if rr.Code != http.StatusOK {
		t.Fatalf("status %d body %s", rr.Code, rr.Body.String())
	}
	if !strings.HasPrefix(rr.Body.String(), "t_ms,message,signal,unit,value\n") {
		t.Fatalf("body %s", rr.Body.String())
	}
	if disp := rr.Header().Get("Content-Disposition"); !strings.Contains(disp, "photon-20260723-212137-00.csv") {
		t.Fatalf("disposition %s", disp)
	}
}
