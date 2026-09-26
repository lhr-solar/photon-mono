package sshd

import (
	"bytes"
	"io"
	"net"
	"os"
	"path/filepath"
	"runtime"
	"testing"
)

func TestParseSCP(t *testing.T) {
	from, path, err := parseSCP("scp -f C:/stream_can_gps_v3")
	if err != nil || !from {
		t.Fatalf("from=%t path=%q err=%v", from, path, err)
	}
	got := windowsPath(path)
	if runtime.GOOS == "windows" && got != `C:\stream_can_gps_v3` {
		t.Fatalf("windows path %q", got)
	}
	from, path, err = parseSCP("scp -p -f /C:/stream_can_gps_v3")
	if err != nil || !from {
		t.Fatalf("p from=%t path=%q err=%v", from, path, err)
	}
}

func TestSCPSendFile(t *testing.T) {
	dir := t.TempDir()
	src := filepath.Join(dir, "payload.bin")
	want := []byte("can-gps-payload")
	if err := os.WriteFile(src, want, 0o644); err != nil {
		t.Fatal(err)
	}
	c1, c2 := net.Pipe()
	t.Cleanup(func() { _ = c1.Close(); _ = c2.Close() })
	errCh := make(chan error, 1)
	go func() {
		errCh <- scpSendFile(c1, src)
	}()
	if _, err := c2.Write([]byte{0}); err != nil {
		t.Fatal(err)
	}
	hdr := make([]byte, 64)
	n, err := c2.Read(hdr)
	if err != nil {
		t.Fatal(err)
	}
	if n < 10 || hdr[0] != 'C' || !bytesHas(hdr[:n], []byte("15 payload.bin\n")) {
		t.Fatalf("hdr %q", hdr[:n])
	}
	if _, err := c2.Write([]byte{0}); err != nil {
		t.Fatal(err)
	}
	got := make([]byte, len(want)+1)
	if _, err := io.ReadFull(c2, got); err != nil {
		t.Fatal(err)
	}
	if string(got[:len(want)]) != string(want) || got[len(want)] != 0 {
		t.Fatalf("data %q", got)
	}
	if _, err := c2.Write([]byte{0}); err != nil {
		t.Fatal(err)
	}
	if err := <-errCh; err != nil {
		t.Fatal(err)
	}
}

func bytesHas(haystack, needle []byte) bool {
	return bytes.Contains(haystack, needle)
}
