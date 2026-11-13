package main

import (
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestUiMcpProxy(t *testing.T) {
	var gotPath, gotAuth, gotCookie, gotBody string
	mcp := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		gotPath = r.URL.Path
		gotAuth = r.Header.Get("Authorization")
		gotCookie = r.Header.Get("Cookie")
		body, _ := io.ReadAll(r.Body)
		gotBody = string(body)
		w.Header().Set("Content-Type", "text/event-stream")
		_, _ = io.WriteString(w, "data: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}\n\n")
	}))
	defer mcp.Close()

	req := httptest.NewRequest(http.MethodPost, "/mcp", strings.NewReader(`{"jsonrpc":"2.0","id":1}`))
	req.Header.Set("Authorization", "Bearer agent-token")
	req.Header.Set("Cookie", "photon_ui=abc; __Host-MCP_CONSENT=yes")
	rec := httptest.NewRecorder()
	uiMcpProxy(mcp.URL).ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("status %d: %s", rec.Code, rec.Body.String())
	}
	if gotPath != "/mcp" || gotBody != `{"jsonrpc":"2.0","id":1}` {
		t.Errorf("forwarded %q with body %q", gotPath, gotBody)
	}
	if gotAuth != "Bearer agent-token" {
		t.Errorf("authorization %q, want the agent's token", gotAuth)
	}
	if gotCookie != "__Host-MCP_CONSENT=yes" {
		t.Errorf("cookie %q, want only the consent cookie", gotCookie)
	}
	if !strings.Contains(rec.Body.String(), `"result"`) {
		t.Errorf("event stream not passed through: %q", rec.Body.String())
	}
}

func TestUiHandlerPasswordGate(t *testing.T) {
	mcp := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = io.WriteString(w, "mcp")
	}))
	defer mcp.Close()
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "photon.html"), []byte("page"), 0o644); err != nil {
		t.Fatal(err)
	}
	handler := uiHandler(dir, "laptop", mcp.URL)

	tests := []struct {
		method, path string
		want         int
		body         string
	}{
		{http.MethodPost, "/mcp", http.StatusOK, "mcp"},
		{http.MethodGet, "/.well-known/oauth-protected-resource/mcp", http.StatusOK, "mcp"},
		{http.MethodGet, "/auth/callback?code=c&state=s", http.StatusOK, "mcp"},
		{http.MethodPost, "/consent", http.StatusOK, "mcp"},
		{http.MethodGet, "/photon.html", http.StatusUnauthorized, ""},
		{http.MethodGet, "/mcp-other", http.StatusUnauthorized, ""},
		// The gate passes this on as a sign-in path; the mux must not serve the page for it.
		{http.MethodGet, "/.well-known/../photon.html", http.StatusTemporaryRedirect, ""},
	}
	for _, tt := range tests {
		t.Run(tt.method+" "+tt.path, func(t *testing.T) {
			rec := httptest.NewRecorder()
			handler.ServeHTTP(rec, httptest.NewRequest(tt.method, tt.path, nil))
			if rec.Code != tt.want {
				t.Errorf("status %d, want %d", rec.Code, tt.want)
			}
			if tt.body != "" && rec.Body.String() != tt.body {
				t.Errorf("body %q, want %q", rec.Body.String(), tt.body)
			}
			if strings.Contains(rec.Body.String(), "page") {
				t.Errorf("page served without the password")
			}
		})
	}
}
