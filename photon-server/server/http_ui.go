package main

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/hex"
	"encoding/json"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"strings"
)

const uiCookieName = "photon_ui"
const kUiCrashMaxBytes = 64 * 1024
const kUiCrashFileMax = 2 * 1024 * 1024

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

func resolveUiDir() string {
	dir := exeDir()
	for _, candidate := range []string{
		filepath.Join(dir, "ui"),
	} {
		if st, err := os.Stat(filepath.Join(candidate, "photon.html")); err == nil && !st.IsDir() {
			return candidate
		}
	}
	return ""
}

func resolveAssetsDir() string {
	return firstExistingDir([]string{
		filepath.Join(exeDir(), "assets"),
	})
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
