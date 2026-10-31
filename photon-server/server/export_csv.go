package main

import (
	"bytes"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
)

// exportToolOverride is the photon-tsdb-export binary tests substitute.
var exportToolOverride string

type csvExportQuery struct {
	Session string
	Level   int
	All     bool
	Sigs    []string
}

func parseCsvExportQuery(v url.Values) (csvExportQuery, error) {
	q := csvExportQuery{Session: v.Get("session"), Level: 2}
	if !validExportSession(q.Session) {
		if q.Session == "" {
			return q, fmt.Errorf("missing session")
		}
		return q, fmt.Errorf("bad session")
	}
	if text := v.Get("level"); text != "" {
		n, err := strconv.Atoi(text)
		if err != nil || n < 0 || n > 5 {
			q.Level = 2
		} else {
			q.Level = n
		}
	}
	signals := v.Get("signals")
	if signals == "all" {
		q.All = true
	} else if signals != "" {
		for _, part := range strings.Split(signals, ",") {
			part = strings.TrimSpace(part)
			if part == "all" {
				q.All = true
			} else if part != "" && len(q.Sigs) < 256 {
				q.Sigs = append(q.Sigs, part)
			}
		}
	}
	for _, sig := range v["sig"] {
		if sig != "" && len(q.Sigs) < 256 {
			q.Sigs = append(q.Sigs, sig)
		}
	}
	return q, nil
}

func validExportSession(id string) bool {
	if id == "" || len(id) > 80 {
		return false
	}
	for i, c := range id {
		ok := (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'
		if !ok || (i == 0 && (c < '0' || c > '9') && (c < 'A' || c > 'Z') && (c < 'a' || c > 'z')) {
			return false
		}
	}
	return !strings.Contains(id, "..")
}

func resolveTsdbExportTool() string {
	if exportToolOverride != "" {
		return exportToolOverride
	}
	exe, err := os.Executable()
	if err != nil {
		return ""
	}
	dir := filepath.Dir(exe)
	for _, name := range []string{"photon-tsdb-export.exe", "photon-tsdb-export"} {
		path := filepath.Join(dir, name)
		if st, err := os.Stat(path); err == nil && !st.IsDir() {
			return path
		}
	}
	return ""
}

func streamTsdbCSV(w http.ResponseWriter, r *http.Request, sess tsdbSession, q csvExportQuery) {
	bin := resolveTsdbExportTool()
	if bin == "" {
		http.Error(w, "CSV export tool is not installed", http.StatusServiceUnavailable)
		return
	}
	args := []string{"--csv", sess.Dir, "--level", strconv.Itoa(q.Level)}
	if q.All || len(q.Sigs) == 0 {
		args = append(args, "--all")
	} else {
		for _, sig := range q.Sigs {
			args = append(args, "--sig", sig)
		}
	}
	cmd := exec.CommandContext(r.Context(), bin, args...)
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		http.Error(w, "export failed", http.StatusInternalServerError)
		return
	}
	stderr, err := cmd.StderrPipe()
	if err != nil {
		http.Error(w, "export failed", http.StatusInternalServerError)
		return
	}
	var errBuf bytes.Buffer
	errDone := make(chan struct{})
	go func() {
		_, _ = io.Copy(&errBuf, stderr)
		close(errDone)
	}()
	if err = cmd.Start(); err != nil {
		http.Error(w, "export failed", http.StatusInternalServerError)
		return
	}
	buf := make([]byte, 32*1024)
	n, readErr := stdout.Read(buf)
	if n == 0 {
		_ = cmd.Wait()
		<-errDone
		msg := strings.TrimSpace(errBuf.String())
		if msg == "" {
			msg = "export failed"
		}
		code := http.StatusInternalServerError
		if strings.Contains(msg, "no matching signals") || strings.Contains(msg, "unknown session") {
			code = http.StatusNotFound
		}
		http.Error(w, msg, code)
		return
	}
	w.Header().Set("Content-Type", "text/csv; charset=utf-8")
	w.Header().Set("Content-Disposition", fmt.Sprintf("attachment; filename=\"photon-%s.csv\"", q.Session))
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(http.StatusOK)
	if _, err = w.Write(buf[:n]); err != nil {
		_ = cmd.Process.Kill()
		_ = cmd.Wait()
		return
	}
	_, _ = io.Copy(w, stdout)
	_ = cmd.Wait()
	<-errDone
	_ = readErr
}
