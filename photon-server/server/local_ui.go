package main

import (
	"bytes"
	"context"
	"crypto/hmac"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/hex"
	"encoding/json"
	"io"
	"log"
	"net"
	"net/http"
	"net/http/httputil"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	DefaultUiHttpPort = 8080
	DefaultUiWsPort   = 8765
	uiCookieName      = "photon_ui"
)

// LocalUI is the PC console: photon-backend + the WASM page, both on loopback.
// It reads this machine's fan-out. It does not write a second TSDB.
type LocalUI struct {
	cmd  *exec.Cmd
	http *http.Server
	done chan struct{}
	logs []*os.File
	mu   sync.Mutex
}

const kUiCrashMaxBytes = 64 * 1024
const kUiCrashFileMax = 2 * 1024 * 1024

func firstExisting(paths []string) string {
	for _, p := range paths {
		if st, err := os.Stat(p); err == nil && !st.IsDir() {
			return p
		}
	}
	return ""
}

func firstExistingDir(paths []string) string {
	for _, p := range paths {
		if st, err := os.Stat(p); err == nil && st.IsDir() {
			return p
		}
	}
	return ""
}

func exeDir() string {
	exe, err := os.Executable()
	if err != nil {
		return ""
	}
	return filepath.Dir(exe)
}

func resolveUiBackend(cfg *Config) string {
	raw := strings.TrimSpace(cfg.Ui)
	if strings.EqualFold(raw, "off") || raw == "-" {
		return ""
	}
	if raw != "" {
		return raw
	}
	dir := exeDir()
	return firstExisting([]string{
		filepath.Join(dir, "photon-backend.exe"),
		filepath.Join(dir, "photon-backend"),
		filepath.Join(dir, "..", "photon-next", ".artifacts", "backend", "photon-backend.exe"),
		filepath.Join(dir, "..", "photon-next", ".artifacts", "photon-backend.exe"),
	})
}

func resolveUiDir() string {
	dir := exeDir()
	for _, candidate := range []string{
		filepath.Join(dir, "ui"),
		filepath.Join(dir, "..", "photon-next", ".artifacts-web"),
	} {
		if st, err := os.Stat(filepath.Join(candidate, "photon.html")); err == nil && !st.IsDir() {
			return candidate
		}
	}
	return ""
}

func resolveAssetsDir() string {
	dir := exeDir()
	return firstExistingDir([]string{
		filepath.Join(dir, "assets"),
		filepath.Join(dir, "..", "photon-next", "assets"),
	})
}

func localBackendArgs(cfg *Config) []string {
	// Live ingest stays daq_tcp. Drive-day logs are a per-client pointer
	// (photon-backend openLog), not the exclusive source. Starting on
	// log_replay used to steal FPGA fan-out and make Live a no-op.
	args := []string{
		"--no-live-tail",
		"--source", "daq_tcp",
		"--daq-host", "127.0.0.1",
		"--daq-port", strconv.Itoa(cfg.ClientPort),
		"--video-host", "127.0.0.1",
		"--video-port", strconv.Itoa(cfg.VideoClientPort),
		"--video-udp-port", strconv.Itoa(cfg.VideoUdpPort),
		"--video-udp-host", "10.66.0.1",
		"--tsdb-dir", cfg.TsdbRoot(),
		"--ws-port", strconv.Itoa(DefaultUiWsPort),
	}
	if assets := resolveAssetsDir(); assets != "" {
		args = append(args, "--assets-dir", assets)
	}
	if logPath := resolveOnTrackPath(); logPath != "" {
		args = append(args, "--log-path", logPath)
	}
	return args
}

func resolveOnTrackPath() string {
	if env := strings.TrimSpace(os.Getenv("PHOTON_ON_TRACK")); env != "" {
		if st, err := os.Stat(filepath.Join(env, "catalog.json")); err == nil && !st.IsDir() {
			return env
		}
	}
	for _, root := range []string{`F:\lhr\_logs_extract\on_track`, filepath.Join(exeDir(), "..", "_logs_extract", "on_track")} {
		if st, err := os.Stat(filepath.Join(root, "catalog.json")); err == nil && !st.IsDir() {
			return root
		}
	}
	return ""
}

func startLocalUI(cfg *Config) *LocalUI {
	bin := resolveUiBackend(cfg)
	if bin == "" {
		log.Printf("[photon-server] local UI not started (pass -ui photon-backend, or off)")
		return nil
	}

	args := localBackendArgs(cfg)
	cmd := exec.Command(bin, args...)
	var logFiles []*os.File
	if dir := exeDir(); dir != "" {
		if f, err := os.OpenFile(filepath.Join(dir, "photon-backend-stdout.log"),
			os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644); err == nil {
			cmd.Stdout = f
			logFiles = append(logFiles, f)
		} else {
			cmd.Stdout = io.Discard
		}
		if f, err := os.OpenFile(filepath.Join(dir, "photon-backend-stderr.log"),
			os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644); err == nil {
			cmd.Stderr = io.MultiWriter(os.Stderr, f)
			logFiles = append(logFiles, f)
		} else {
			cmd.Stderr = os.Stderr
		}
	} else {
		cmd.Stdout = io.Discard
		cmd.Stderr = os.Stderr
	}
	if cfg.Password != "" {
		cmd.Env = append(os.Environ(), "PHOTON_PASSWORD="+cfg.Password)
	}
	if err := cmd.Start(); err != nil {
		log.Printf("[photon-server] local UI backend start failed: %v", err)
		for _, f := range logFiles {
			_ = f.Close()
		}
		return nil
	}
	ui := &LocalUI{cmd: cmd, done: make(chan struct{}), logs: logFiles}
	go func() {
		_ = cmd.Wait()
		close(ui.done)
	}()
	log.Printf("[photon-server] local UI backend pid=%d ws://127.0.0.1:%d daq 127.0.0.1:%d",
		cmd.Process.Pid, DefaultUiWsPort, cfg.ClientPort)

	uiDir := resolveUiDir()
	if uiDir == "" {
		log.Printf("[photon-server] local UI page missing (expected photon.html next to the binary or in photon-next/.artifacts-web)")
		return ui
	}

	mux := http.NewServeMux()
	mux.Handle("/crash", uiCrashHandler(uiCrashLogPath()))
	mux.Handle("/ws", uiWsProxy())
	mux.Handle("/export.csv", uiExportProxy())
	mux.Handle("/", uiFileHandler(uiDir))
	addr := net.JoinHostPort("127.0.0.1", strconv.Itoa(DefaultUiHttpPort))
	handler := http.Handler(mux)
	if cfg.Password != "" {
		handler = uiPasswordGate(cfg.Password, mux)
	}
	srv := &http.Server{Addr: addr, Handler: handler}
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		log.Printf("[photon-server] local UI http %s: %v", addr, err)
		return ui
	}
	ui.http = srv
	go func() {
		if err := srv.Serve(ln); err != nil && err != http.ErrServerClosed {
			log.Printf("[photon-server] local UI http: %v", err)
		}
	}()
	url := "http://127.0.0.1:" + strconv.Itoa(DefaultUiHttpPort) + "/photon.html"
	log.Printf("[photon-server] local UI %s (password on HTTP; /ws proxies loopback :%d)", url, DefaultUiWsPort)
	openBrowser(url)
	return ui
}

func uiCookieValue(password string) string {
	mac := hmac.New(sha256.New, []byte(password))
	mac.Write([]byte("photon-ui-v1"))
	return hex.EncodeToString(mac.Sum(nil))
}

func uiCookieOK(r *http.Request, password string) bool {
	c, err := r.Cookie(uiCookieName)
	if err != nil {
		return false
	}
	want := uiCookieValue(password)
	return subtle.ConstantTimeCompare([]byte(c.Value), []byte(want)) == 1
}

func uiPasswordOK(got, want string) bool {
	if want == "" {
		return true
	}
	return subtle.ConstantTimeCompare([]byte(got), []byte(want)) == 1
}

// Scripts, wasm, and fonts must load without a second password prompt.
// Browsers often omit Basic auth on fetch()/WebAssembly; a 401 leaves a black page.
func uiPublicAsset(path string) bool {
	p := strings.ToLower(path)
	if strings.HasPrefix(p, "/models/") {
		return true
	}
	switch filepath.Ext(p) {
	case ".js", ".wasm", ".data", ".css", ".png", ".jpg", ".jpeg", ".svg", ".glb", ".gltf", ".ttf", ".woff", ".woff2", ".map":
		return true
	default:
		return false
	}
}

func uiPasswordGate(password string, next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if uiPublicAsset(r.URL.Path) {
			next.ServeHTTP(w, r)
			return
		}
		if r.URL.Path == "/crash" && r.Method == http.MethodPost {
			next.ServeHTTP(w, r)
			return
		}
		if uiCookieOK(r, password) {
			next.ServeHTTP(w, r)
			return
		}
		_, pass, ok := r.BasicAuth()
		if ok && uiPasswordOK(pass, password) {
			http.SetCookie(w, &http.Cookie{
				Name:     uiCookieName,
				Value:    uiCookieValue(password),
				Path:     "/",
				HttpOnly: true,
				SameSite: http.SameSiteLaxMode,
			})
			next.ServeHTTP(w, r)
			return
		}
		w.Header().Set("WWW-Authenticate", `Basic realm="Photon"`)
		http.Error(w, "password required", http.StatusUnauthorized)
	})
}

func uiFileHandler(uiDir string) http.Handler {
	fs := http.FileServer(http.Dir(uiDir))
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		p := r.URL.Path
		if p == "/" || p == "" {
			http.Redirect(w, r, "/photon.html", http.StatusFound)
			return
		}
		switch {
		case strings.HasSuffix(strings.ToLower(p), ".wasm"):
			w.Header().Set("Content-Type", "application/wasm")
		case strings.HasSuffix(strings.ToLower(p), ".data"):
			w.Header().Set("Content-Type", "application/octet-stream")
		}
		ext := strings.ToLower(filepath.Ext(p))
		switch ext {
		case ".html", ".js", ".wasm", ".data":
			w.Header().Set("Cache-Control", "no-store")
		}
		fs.ServeHTTP(w, r)
	})
}

func uiLoopbackProxy(rewriteRoot bool) http.Handler {
	target, err := url.Parse("http://127.0.0.1:" + strconv.Itoa(DefaultUiWsPort))
	if err != nil {
		return http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
			http.Error(w, "backend proxy", http.StatusBadGateway)
		})
	}
	proxy := httputil.NewSingleHostReverseProxy(target)
	proxy.FlushInterval = 100 * time.Millisecond
	if rewriteRoot {
		proxy.FlushInterval = -1
	}
	director := proxy.Director
	proxy.Director = func(r *http.Request) {
		director(r)
		if rewriteRoot {
			r.URL.Path = "/"
			r.URL.RawPath = "/"
		}
		r.Host = target.Host
	}
	return proxy
}

func uiWsProxy() http.Handler {
	return uiLoopbackProxy(true)
}

func uiExportProxy() http.Handler {
	return uiLoopbackProxy(false)
}

func openBrowser(url string) {
	var cmd *exec.Cmd
	if runtime.GOOS == "windows" {
		cmd = exec.Command("rundll32", "url.dll,FileProtocolHandler", url)
	} else {
		cmd = exec.Command("xdg-open", url)
	}
	if err := cmd.Start(); err != nil {
		log.Printf("[photon-server] local UI open: %v", err)
	}
}

func (u *LocalUI) Stop() {
	if u == nil {
		return
	}
	if u.http != nil {
		ctx, cancel := context.WithTimeout(context.Background(), time.Second)
		_ = u.http.Shutdown(ctx)
		cancel()
	}
	u.mu.Lock()
	cmd := u.cmd
	u.mu.Unlock()
	if cmd != nil && cmd.Process != nil {
		_ = cmd.Process.Kill()
		<-u.done
	}
	for _, f := range u.logs {
		_ = f.Close()
	}
	log.Printf("[photon-server] local UI stopped")
}

func uiCrashLogPath() string {
	if d := exeDir(); d != "" {
		return filepath.Join(d, "photon-ui-crash.log")
	}
	return "photon-ui-crash.log"
}

func uiCrashHandler(logPath string) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.Method {
		case http.MethodPost:
			r.Body = http.MaxBytesReader(w, r.Body, kUiCrashMaxBytes)
			body, err := io.ReadAll(r.Body)
			if err != nil {
				http.Error(w, "too large", http.StatusRequestEntityTooLarge)
				return
			}
			body = bytes.TrimSpace(body)
			if len(body) == 0 {
				http.Error(w, "empty", http.StatusBadRequest)
				return
			}
			if !json.Valid(body) {
				wrapped, _ := json.Marshal(map[string]string{"raw": string(body)})
				body = wrapped
			}
			if err := appendUiCrash(logPath, body); err != nil {
				log.Printf("[photon-server] ui crash log: %v", err)
				http.Error(w, "log failed", http.StatusInternalServerError)
				return
			}
			preview := string(body)
			if len(preview) > 240 {
				preview = preview[:240]
			}
			log.Printf("[photon-server] ui crash: %s", preview)
			w.WriteHeader(http.StatusNoContent)
		case http.MethodGet:
			b, err := os.ReadFile(logPath)
			if err != nil {
				if os.IsNotExist(err) {
					w.Header().Set("Content-Type", "text/plain; charset=utf-8")
					w.WriteHeader(http.StatusOK)
					return
				}
				http.Error(w, "read failed", http.StatusInternalServerError)
				return
			}
			if len(b) > 32*1024 {
				b = b[len(b)-32*1024:]
			}
			w.Header().Set("Content-Type", "text/plain; charset=utf-8")
			_, _ = w.Write(b)
		default:
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		}
	})
}

func appendUiCrash(logPath string, body []byte) error {
	if st, err := os.Stat(logPath); err == nil && st.Size() > kUiCrashFileMax {
		_ = os.Rename(logPath, logPath+".old")
	}
	f, err := os.OpenFile(logPath, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644)
	if err != nil {
		return err
	}
	defer f.Close()
	if _, err := f.Write(append(body, '\n')); err != nil {
		return err
	}
	return nil
}
