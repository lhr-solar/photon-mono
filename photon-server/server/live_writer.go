package main

import (
	"io"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
)

// LiveWriter is the TSDB process photon-server starts on this PC.
// Oracle is only a pipe; this is where telemetry is stored.
type LiveWriter struct {
	cmd  *exec.Cmd
	done chan struct{}
	mu   sync.Mutex
}

func resolveLiveWriter(cfg *Config) string {
	raw := strings.TrimSpace(cfg.LiveWriter)
	if strings.EqualFold(raw, "off") || raw == "-" {
		return ""
	}
	if raw != "" {
		return raw
	}
	exe, err := os.Executable()
	if err != nil {
		return ""
	}
	dir := filepath.Dir(exe)
	candidates := []string{
		filepath.Join(dir, "photon-index.exe"),
		filepath.Join(dir, "photon-index"),
		filepath.Join(dir, "..", "photon-next", ".artifacts", "backend", "photon-index.exe"),
		filepath.Join(dir, "..", "photon-next", ".artifacts", "photon-index.exe"),
	}
	for _, c := range candidates {
		if st, err := os.Stat(c); err == nil && !st.IsDir() {
			return c
		}
	}
	return ""
}

func resolveDbc(cfg *Config) string {
	if strings.TrimSpace(cfg.DbcPath) != "" {
		return cfg.DbcPath
	}
	exe, err := os.Executable()
	if err != nil {
		return ""
	}
	dir := filepath.Dir(exe)
	candidates := []string{
		filepath.Join(dir, "assets", "dbc", "carcan_telemcan.dbc"),
		filepath.Join(dir, "..", "photon-next", "assets", "dbc", "carcan_telemcan.dbc"),
	}
	for _, c := range candidates {
		if st, err := os.Stat(c); err == nil && !st.IsDir() {
			return c
		}
	}
	return ""
}

func startLiveWriter(cfg *Config) *LiveWriter {
	bin := resolveLiveWriter(cfg)
	if bin == "" {
		log.Printf("[photon-server] TSDB live writer not started (pass -live-writer photon-index, or off)")
		return nil
	}
	dbc := resolveDbc(cfg)
	if dbc == "" {
		log.Printf("[photon-server] TSDB live writer skipped: no DBC (pass -dbc)")
		return nil
	}
	if err := os.MkdirAll(cfg.TsdbRoot(), 0o755); err != nil {
		log.Printf("[photon-server] TSDB dir %s: %v", cfg.TsdbRoot(), err)
		return nil
	}

	args := []string{
		"--live-fanout", cfg.LiveFanoutAddr(),
		"--out", cfg.TsdbRoot(),
		"--dbc", dbc,
		"--video-vpt", cfg.VideoVptPath(),
	}
	cmd := exec.Command(bin, args...)
	cmd.Stdout = io.Discard
	cmd.Stderr = os.Stderr
	if cfg.Password != "" {
		cmd.Env = append(os.Environ(), "PHOTON_PASSWORD="+cfg.Password)
	}
	if err := cmd.Start(); err != nil {
		log.Printf("[photon-server] TSDB live writer start failed: %v", err)
		return nil
	}
	log.Printf("[photon-server] TSDB live writer pid=%d %s -> %s", cmd.Process.Pid, cfg.LiveFanoutAddr(), cfg.TsdbRoot())
	w := &LiveWriter{cmd: cmd, done: make(chan struct{})}
	go func() {
		_ = cmd.Wait()
		close(w.done)
	}()
	return w
}

func (w *LiveWriter) Stop() {
	if w == nil {
		return
	}
	w.mu.Lock()
	cmd := w.cmd
	w.mu.Unlock()
	if cmd == nil || cmd.Process == nil {
		return
	}
	_ = cmd.Process.Kill()
	<-w.done
	log.Printf("[photon-server] TSDB live writer stopped")
}
