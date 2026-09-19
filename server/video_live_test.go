package main

import (
	"context"
	"net"
	"testing"
	"time"
)

func TestWriteVideoLiveDoesNotHammerKeyframe(t *testing.T) {
	dir := t.TempDir()
	store, err := OpenVideoStore(dir)
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	defer store.Close()

	base := videoNowMS()
	mustAppend(t, store, annexBNAL(nalTypeSPS, 0x42, 0x00, 0x0a), base)
	mustAppend(t, store, annexBNAL(nalTypePPS, 0xce, 0x38), base)
	mustAppend(t, store, annexBNAL(nalTypeIDR, 0x01, 0x77), base)
	for i := 1; i <= videoLiveMaxLag+20; i++ {
		mustAppend(t, store, annexBNAL(1, byte(i)), base+uint64(i)*33)
	}

	client, server := net.Pipe()
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go writeVideoLive(ctx, server, store)

	keys := 0
	dupKeys := 0
	var lastKeyTs uint64
	deadline := time.Now().Add(400 * time.Millisecond)
	for time.Now().Before(deadline) {
		_ = client.SetReadDeadline(time.Now().Add(120 * time.Millisecond))
		frame, err := DecodeVideoFrame(client)
		if err != nil {
			break
		}
		if frame.IsConfig() {
			continue
		}
		if frame.IsKeyframe() {
			if keys > 0 && frame.TimestampMs == lastKeyTs {
				dupKeys++
			}
			lastKeyTs = frame.TimestampMs
			keys++
		}
	}
	cancel()
	_ = client.Close()

	if keys != 1 {
		t.Fatalf("live sent %d keyframes, want the seed IDR once", keys)
	}
	if dupKeys != 0 {
		t.Fatalf("live re-sent the same IDR %d extra times", dupKeys)
	}
}
