package main

import (
	"bytes"
	"io"
	"net"
	"testing"
	"time"
)

func TestClientAuthRoundTrip(t *testing.T) {
	var buf bytes.Buffer
	if err := WriteClientAuth(&buf, "secret"); err != nil {
		t.Fatal(err)
	}
	got, err := ReadClientAuth(&buf)
	if err != nil {
		t.Fatal(err)
	}
	if got != "secret" {
		t.Fatalf("got %q", got)
	}
}

func TestClientAuthRejectsBadMagic(t *testing.T) {
	if _, err := ReadClientAuth(bytes.NewReader([]byte("CAN1xxxx"))); err == nil {
		t.Fatal("expected reject")
	}
}

func TestGateWrongPassword(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	gate := NewClientGate("right")
	done := make(chan error, 1)
	go func() {
		c, err := ln.Accept()
		if err != nil {
			done <- err
			return
		}
		defer c.Close()
		if err := gate.Admit(c); err != nil {
			done <- err
			return
		}
		done <- gate.Auth(c)
		gate.Release(c)
	}()
	c, err := net.Dial("tcp", ln.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	if err := WriteClientAuth(c, "wrong"); err != nil {
		t.Fatal(err)
	}
	select {
	case err := <-done:
		if err == nil {
			t.Fatal("wrong password should fail")
		}
	case <-time.After(2 * time.Second):
		t.Fatal("timeout")
	}
}

func TestGatePerIPCap(t *testing.T) {
	gate := NewClientGate("x")
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	var held []net.Conn
	defer func() {
		for _, c := range held {
			_ = c.Close()
		}
	}()
	for i := 0; i < maxClientConnsPerIP; i++ {
		if i == maxAcceptsPerIPPerSec {
			time.Sleep(1100 * time.Millisecond)
		}
		srv, cli := acceptPair(t, ln)
		held = append(held, srv, cli)
		if err := gate.Admit(remoteAddrConn{srv, "10.1.2.3:9"}); err != nil {
			t.Fatalf("admit %d: %v", i, err)
		}
	}
	extraSrv, extraCli := acceptPair(t, ln)
	held = append(held, extraSrv, extraCli)
	if err := gate.Admit(remoteAddrConn{extraSrv, "10.1.2.3:9"}); err != errClientBusy {
		t.Fatalf("expected busy, got %v", err)
	}
}

func TestGateLoopbackSkipsPerIPCap(t *testing.T) {
	gate := NewClientGate("x")
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	var held []net.Conn
	defer func() {
		for _, c := range held {
			_ = c.Close()
		}
	}()
	want := maxClientConnsPerIP + 2
	for i := 0; i < want; i++ {
		if i > 0 && i%maxAcceptsPerIPPerSec == 0 {
			time.Sleep(1100 * time.Millisecond)
		}
		srv, cli := acceptPair(t, ln)
		held = append(held, srv, cli)
		if err := gate.Admit(srv); err != nil {
			t.Fatalf("loopback admit %d: %v", i, err)
		}
	}
}

func TestGateWireGuardSkipsPerIPCap(t *testing.T) {
	gate := NewClientGate("x")
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	var held []net.Conn
	defer func() {
		for _, c := range held {
			_ = c.Close()
		}
	}()
	want := maxClientConnsPerIP + 2
	for i := 0; i < want; i++ {
		srv, cli := acceptPair(t, ln)
		held = append(held, srv, cli)
		if err := gate.Admit(remoteAddrConn{srv, "10.66.0.1:9"}); err != nil {
			t.Fatalf("wg admit %d: %v", i, err)
		}
	}
}

type tcpStrAddr string

func (a tcpStrAddr) Network() string { return "tcp" }
func (a tcpStrAddr) String() string  { return string(a) }

type remoteAddrConn struct {
	net.Conn
	remote string
}

func (c remoteAddrConn) RemoteAddr() net.Addr { return tcpStrAddr(c.remote) }

func acceptPair(t *testing.T, ln net.Listener) (net.Conn, net.Conn) {
	t.Helper()
	errCh := make(chan error, 1)
	var srv net.Conn
	go func() {
		var err error
		srv, err = ln.Accept()
		errCh <- err
	}()
	cli, err := net.Dial("tcp", ln.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	if err := <-errCh; err != nil {
		t.Fatal(err)
	}
	return srv, cli
}

func TestReadClientAuthEmpty(t *testing.T) {
	if _, err := ReadClientAuth(bytes.NewReader(nil)); err != io.EOF && err != io.ErrUnexpectedEOF {
		t.Fatalf("got %v", err)
	}
}
