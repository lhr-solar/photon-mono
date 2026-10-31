package sshd

import (
	"bytes"
	"crypto/ed25519"
	"crypto/rand"
	"io"
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"golang.org/x/crypto/ssh"
)

func TestKeyAuthExec(t *testing.T) {
	dir := t.TempDir()
	pub, signer := mustKey(t)
	keysPath := filepath.Join(dir, "authorized_keys")
	if err := os.WriteFile(keysPath, ssh.MarshalAuthorizedKey(pub), 0o600); err != nil {
		t.Fatal(err)
	}

	srv, err := New(Config{
		Bind:        "127.0.0.1:0",
		HostKeyPath: filepath.Join(dir, "host"),
		AuthKeys:    keysPath,
	})
	if err != nil {
		t.Fatal(err)
	}
	if err := srv.Start(); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = srv.Close() })

	out, code := sshRun(t, srv.Addr(), signer, "", "echo photon-sshd-ok")
	if code != 0 {
		t.Fatalf("exit %d out=%q", code, out)
	}
	if !strings.Contains(out, "photon-sshd-ok") {
		t.Fatalf("output %q", out)
	}
}

func TestUnknownKeyRejected(t *testing.T) {
	dir := t.TempDir()
	pub, _ := mustKey(t)
	_, other := mustKey(t)
	keysPath := filepath.Join(dir, "authorized_keys")
	if err := os.WriteFile(keysPath, ssh.MarshalAuthorizedKey(pub), 0o600); err != nil {
		t.Fatal(err)
	}
	srv, err := New(Config{
		Bind:        "127.0.0.1:0",
		HostKeyPath: filepath.Join(dir, "host"),
		AuthKeys:    keysPath,
	})
	if err != nil {
		t.Fatal(err)
	}
	if err := srv.Start(); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = srv.Close() })

	_, err = sshDial(srv.Addr(), other, "")
	if err == nil {
		t.Fatal("expected unknown key to fail")
	}
}

func TestPasswordAuth(t *testing.T) {
	dir := t.TempDir()
	srv, err := New(Config{
		Bind:        "127.0.0.1:0",
		HostKeyPath: filepath.Join(dir, "host"),
		Password:    "laptop",
	})
	if err != nil {
		t.Fatal(err)
	}
	if err := srv.Start(); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = srv.Close() })

	out, code := sshRun(t, srv.Addr(), nil, "laptop", "echo pw-ok")
	if code != 0 || !strings.Contains(out, "pw-ok") {
		t.Fatalf("exit %d out=%q", code, out)
	}
	_, err = sshDial(srv.Addr(), nil, "wrong")
	if err == nil {
		t.Fatal("expected bad password to fail")
	}
}

func TestRequiresAuth(t *testing.T) {
	_, err := New(Config{
		Bind:        "127.0.0.1:0",
		HostKeyPath: filepath.Join(t.TempDir(), "host"),
	})
	if err == nil {
		t.Fatal("expected missing auth to fail")
	}
}

func TestDirectTcpipLoopback(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = ln.Close() })
	go func() {
		c, err := ln.Accept()
		if err != nil {
			return
		}
		defer c.Close()
		buf := make([]byte, 64)
		n, err := c.Read(buf)
		if err != nil {
			return
		}
		_, _ = c.Write(buf[:n])
	}()

	srv, signer := startKeyServer(t)
	client, err := sshDial(srv.Addr(), signer, "")
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()

	conn, err := client.Dial("tcp", ln.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	if _, err := conn.Write([]byte("ping")); err != nil {
		t.Fatal(err)
	}
	got := make([]byte, 4)
	if _, err := io.ReadFull(conn, got); err != nil {
		t.Fatal(err)
	}
	if string(got) != "ping" {
		t.Fatalf("got %q", got)
	}
}

func TestDirectTcpipRejectsNonLoopback(t *testing.T) {
	srv, signer := startKeyServer(t)
	client, err := sshDial(srv.Addr(), signer, "")
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()

	conn, err := client.Dial("tcp", "1.1.1.1:80")
	if err == nil {
		_ = conn.Close()
		t.Fatal("expected non-loopback forward to fail")
	}
}

func TestLoopbackTCPAddr(t *testing.T) {
	ok, err := loopbackTCPAddr("127.0.0.1", 5400)
	if err != nil || ok != "127.0.0.1:5400" {
		t.Fatalf("loopback: %q %v", ok, err)
	}
	ok, err = loopbackTCPAddr("localhost", 8080)
	if err != nil || ok != "127.0.0.1:8080" {
		t.Fatalf("localhost: %q %v", ok, err)
	}
	if _, err := loopbackTCPAddr("8.8.8.8", 53); err == nil {
		t.Fatal("expected non-loopback IP to fail")
	}
	if _, err := loopbackTCPAddr("example.com", 443); err == nil {
		t.Fatal("expected hostname to fail")
	}
	if _, err := loopbackTCPAddr("127.0.0.1", 0); err == nil {
		t.Fatal("expected port 0 to fail")
	}
}

func startKeyServer(t *testing.T) (*Server, ssh.Signer) {
	t.Helper()
	dir := t.TempDir()
	pub, signer := mustKey(t)
	keysPath := filepath.Join(dir, "authorized_keys")
	if err := os.WriteFile(keysPath, ssh.MarshalAuthorizedKey(pub), 0o600); err != nil {
		t.Fatal(err)
	}
	srv, err := New(Config{
		Bind:        "127.0.0.1:0",
		HostKeyPath: filepath.Join(dir, "host"),
		AuthKeys:    keysPath,
	})
	if err != nil {
		t.Fatal(err)
	}
	if err := srv.Start(); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = srv.Close() })
	return srv, signer
}

func mustKey(t *testing.T) (ssh.PublicKey, ssh.Signer) {
	t.Helper()
	_, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	signer, err := ssh.NewSignerFromKey(priv)
	if err != nil {
		t.Fatal(err)
	}
	return signer.PublicKey(), signer
}

func sshDial(addr string, key ssh.Signer, password string) (*ssh.Client, error) {
	cfg := &ssh.ClientConfig{
		User:            "alper",
		HostKeyCallback: ssh.InsecureIgnoreHostKey(),
		Timeout:         3 * time.Second,
	}
	if key != nil {
		cfg.Auth = append(cfg.Auth, ssh.PublicKeys(key))
	}
	if password != "" {
		cfg.Auth = append(cfg.Auth, ssh.Password(password))
	}
	return ssh.Dial("tcp", addr, cfg)
}

func sshRun(t *testing.T, addr string, key ssh.Signer, password, cmd string) (string, int) {
	t.Helper()
	client, err := sshDial(addr, key, password)
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	sess, err := client.NewSession()
	if err != nil {
		t.Fatal(err)
	}
	defer sess.Close()
	stdout, err := sess.StdoutPipe()
	if err != nil {
		t.Fatal(err)
	}
	stderr, err := sess.StderrPipe()
	if err != nil {
		t.Fatal(err)
	}
	var buf bytes.Buffer
	if err := sess.Start(cmd); err != nil {
		t.Fatal(err)
	}
	_, _ = io.Copy(&buf, io.MultiReader(stdout, stderr))
	code := 0
	if err := sess.Wait(); err != nil {
		if ee, ok := err.(*ssh.ExitError); ok {
			code = ee.ExitStatus()
		} else {
			t.Fatal(err)
		}
	}
	return buf.String(), code
}
