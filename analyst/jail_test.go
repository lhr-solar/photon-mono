package analyst

import (
	"os"
	"path/filepath"
	"testing"
)

func TestJailIsolationAndDestroy(t *testing.T) {
	root := t.TempDir()
	a, err := NewJail(root, "client-a", "job-1")
	if err != nil {
		t.Fatal(err)
	}
	b, err := NewJail(root, "client-b", "job-1")
	if err != nil {
		t.Fatal(err)
	}
	if a.Dir == b.Dir {
		t.Fatal("shared jail dir")
	}
	src := filepath.Join(t.TempDir(), "bundle.json")
	if err := os.WriteFile(src, []byte(`{"sessionId":"a"}`), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := a.InstallBundle(src); err != nil {
		t.Fatal(err)
	}
	if _, err := b.Confine(a.BundlePath()); err == nil {
		t.Fatal("client B confined into client A jail")
	}
	dir := a.Dir
	if err := a.Destroy(); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(dir); !os.IsNotExist(err) {
		t.Fatalf("jail leftover: %v", err)
	}
}

func TestJailRejectsBadIDs(t *testing.T) {
	root := t.TempDir()
	if _, err := NewJail(root, "../x", "job"); err == nil {
		t.Fatal("expected path escape reject")
	}
	if _, err := NewJail(root, "ok", "job/../x"); err == nil {
		t.Fatal("expected path escape reject")
	}
}

func TestLooksLikeStore(t *testing.T) {
	if !looksLikeStore(filepath.Join("data", "tsdb", "sess")) {
		t.Fatal("tsdb")
	}
	if !looksLikeStore(filepath.Join("data", "video", "segments")) {
		t.Fatal("video")
	}
	if looksLikeStore(filepath.Join("tmp", "grok-analyst", "jobs", "c", "j")) {
		t.Fatal("scratch should be allowed")
	}
}
