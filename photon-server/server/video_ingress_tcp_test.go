package main

import (
	"context"
	"encoding/binary"
	"net"
	"sync"
	"testing"
	"time"
)

func TestListenVideoIngressTCPWritesStore(t *testing.T) {
	dir := t.TempDir()
	store, err := OpenVideoStore(dir)
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	defer store.Close()

	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	defer ln.Close()

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	var wg sync.WaitGroup
	go listenVideoIngressTCP(ctx, ln, store, nil, &wg)

	pkt := buildRTP(1, 90000, true, []byte{0x65, 0xAA, 0xBB, 0xCC})
	conn, err := net.Dial("tcp", ln.Addr().String())
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	var hdr [4]byte
	binary.BigEndian.PutUint32(hdr[:], uint32(len(pkt)))
	if _, err := conn.Write(hdr[:]); err != nil {
		t.Fatalf("write len: %v", err)
	}
	if _, err := conn.Write(pkt); err != nil {
		t.Fatalf("write rtp: %v", err)
	}

	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if store.Stats().BytesStored > 0 {
			_ = conn.Close()
			cancel()
			_ = ln.Close()
			wg.Wait()
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	_ = conn.Close()
	t.Fatalf("store stayed empty after TCP RTP write")
}
