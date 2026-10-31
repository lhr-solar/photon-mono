package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"io"
	"net"
	"testing"
	"time"
)

func TestVideoPointersReuseAuthenticatedConnection(t *testing.T) {
	store, err := OpenVideoStore(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	mustAppend(t, store, annexBNAL(nalTypeSPS, 0x42, 0x00, 0x0a), 1000)
	mustAppend(t, store, annexBNAL(nalTypePPS, 0xce, 0x38), 1000)
	mustAppend(t, store, annexBNAL(nalTypeIDR, 0x80, 0x77), 1000)
	mustAppend(t, store, annexBNAL(1, 0x80, 0x55), 1033)
	row := store.Locate(0, 1000, 0).Entry
	want, err := store.ReadRange(row.SegmentID, row.ByteOffset, row.ByteLength)
	if err != nil || len(want) == 0 {
		t.Fatalf("fixture: %v", err)
	}
	gate := NewClientGate("test-password")
	client, server := net.Pipe()
	defer client.Close()
	done := make(chan struct{})
	go func() {
		defer close(done)
		defer server.Close()
		if !admitAndAuth(gate, server) {
			return
		}
		defer gate.Release(server)
		serveVideoClient(context.Background(), server, store)
	}()
	_ = client.SetDeadline(time.Now().Add(3 * time.Second))
	if err := WriteClientAuth(client, "test-password"); err != nil {
		t.Fatal(err)
	}
	// More operations than the per-second connection limit, with one auth.
	for i := 0; i < 12; i++ {
		req := VideoRequestExt{Mode: VideoModeLocate, StartTimeMs: 1000, Prefetch: 4}
		if i%2 == 1 {
			req = VideoRequestExt{Mode: VideoModeRange, SegmentID: row.SegmentID, Offset: row.ByteOffset, Length: row.ByteLength}
		}
		if err := EncodeVideoRequestExt(client, req); err != nil {
			t.Fatalf("request %d: %v", i, err)
		}
		var size [4]byte
		if _, err := io.ReadFull(client, size[:]); err != nil {
			t.Fatalf("header %d: %v", i, err)
		}
		body := make([]byte, binary.BigEndian.Uint32(size[:]))
		if _, err := io.ReadFull(client, body); err != nil {
			t.Fatalf("body %d: %v", i, err)
		}
		if i%2 == 1 && !bytes.Equal(body, want) {
			t.Fatalf("range %d corrupted", i)
		}
		if i%2 == 0 && (len(body) == 0 || body[0] != videoLocateStatusOK) {
			t.Fatalf("locate %d failed", i)
		}
	}
	client.Close()
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("disconnected RPC did not release its slot")
	}
	if gate.open != 0 {
		t.Fatalf("leaked gate slot: %d", gate.open)
	}
}

func TestVideoLocateAfterEndDoesNotReturnStaleClip(t *testing.T) {
	store, err := OpenVideoStore(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	mustAppend(t, store, annexBNAL(nalTypeSPS, 0x42, 0x00, 0x0a), 1000)
	mustAppend(t, store, annexBNAL(nalTypePPS, 0xce, 0x38), 1000)
	mustAppend(t, store, annexBNAL(nalTypeIDR, 0x80, 0x77), 1000)
	mustAppend(t, store, annexBNAL(1, 0x80, 0x55), 1450)
	if !store.Locate(0, 1200, 4).Found {
		t.Fatal("recorded cursor should resolve")
	}
	miss := store.Locate(0, 60_000, 4)
	if miss.Found || len(miss.Prefetch) != 0 {
		t.Fatal("out-of-range cursor returned stale GOP")
	}
	if miss.MaxMs != 1450 || len(miss.Coverage) == 0 {
		t.Fatal("miss lost recording bounds")
	}
}

func TestVideoLocateGapDoesNotReturnStaleClip(t *testing.T) {
	store, err := OpenVideoStore(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	mustAppend(t, store, annexBNAL(nalTypeSPS, 0x42, 0x00, 0x0a), 1000)
	mustAppend(t, store, annexBNAL(nalTypePPS, 0xce, 0x38), 1000)
	mustAppend(t, store, annexBNAL(nalTypeIDR, 0x80, 0x77), 1000)
	mustAppend(t, store, annexBNAL(1, 0x80, 0x55), 1450)
	mustAppend(t, store, annexBNAL(nalTypeSPS, 0x42, 0x00, 0x0a), 100_000)
	mustAppend(t, store, annexBNAL(nalTypePPS, 0xce, 0x38), 100_000)
	mustAppend(t, store, annexBNAL(nalTypeIDR, 0x80, 0x11), 100_000)
	mustAppend(t, store, annexBNAL(1, 0x80, 0x22), 100_450)
	if !store.Locate(0, 1200, 4).Found {
		t.Fatal("first clip should resolve")
	}
	if !store.Locate(0, 100_100, 4).Found {
		t.Fatal("resumed clip should resolve")
	}
	newest := store.Locate(0, 0, 4)
	if !newest.Found {
		t.Fatal("newest should resolve")
	}
	if newest.Entry.StartMs < 100_000 {
		t.Fatalf("locate 0 returned start %d, want the resumed clip", newest.Entry.StartMs)
	}
	miss := store.Locate(0, 50_000, 4)
	if miss.Found || len(miss.Prefetch) != 0 {
		t.Fatal("gap cursor returned stale GOP")
	}
	if miss.MaxMs != 100_450 || len(miss.Coverage) == 0 {
		t.Fatal("gap miss lost recording bounds")
	}
	// A 350 ms inter-GOP hole is a frame-interval gap, not a recording hole.
	store2, err := OpenVideoStore(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	defer store2.Close()
	mustAppend(t, store2, annexBNAL(nalTypeSPS, 0x42, 0x00, 0x0a), 1000)
	mustAppend(t, store2, annexBNAL(nalTypePPS, 0xce, 0x38), 1000)
	mustAppend(t, store2, annexBNAL(nalTypeIDR, 0x80, 0x77), 1000)
	mustAppend(t, store2, annexBNAL(1, 0x80, 0x55), 1450)
	mustAppend(t, store2, annexBNAL(nalTypeIDR, 0x80, 0x11), 1800)
	mustAppend(t, store2, annexBNAL(1, 0x80, 0x22), 2200)
	if !store2.Locate(0, 1600, 4).Found {
		t.Fatal("short GOP gap should still resolve")
	}
}
