package main

import (
	"bytes"
	"os"
	"testing"
)

func TestStreamStoreAppendReadAndRecover(t *testing.T) {
	tempDir, err := os.MkdirTemp("", "photon-store-test-*")
	if err != nil {
		t.Fatalf("failed to create temp dir: %v", err)
	}
	defer os.RemoveAll(tempDir)

	store, err := OpenStreamStore(tempDir)
	if err != nil {
		t.Fatalf("failed to open stream store: %v", err)
	}

	payload1 := []byte("can-test-packet-1")
	payload2 := []byte("can-test-packet-2")

	id1, err := store.Append(payload1)
	if err != nil {
		t.Fatalf("failed to append payload1: %v", err)
	}
	if id1 != 1 {
		t.Errorf("expected id1 = 1, got %d", id1)
	}

	id2, err := store.Append(payload2)
	if err != nil {
		t.Fatalf("failed to append payload2: %v", err)
	}
	if id2 != 2 {
		t.Errorf("expected id2 = 2, got %d", id2)
	}

	// Read message 1
	msg1, ok, err := store.Read(id1)
	if err != nil || !ok {
		t.Fatalf("failed to read id1: ok=%v, err=%v", ok, err)
	}
	if !bytes.Equal(msg1.Payload, payload1) {
		t.Errorf("payload mismatch: expected %s, got %s", payload1, msg1.Payload)
	}

	// Read message 2 via ReadOrWait
	msg2, err := store.ReadOrWait(id2)
	if err != nil {
		t.Fatalf("failed to ReadOrWait id2: %v", err)
	}
	if !bytes.Equal(msg2.Payload, payload2) {
		t.Errorf("payload mismatch: expected %s, got %s", payload2, msg2.Payload)
	}

	stats := store.Stats()
	if stats.LatestID != 2 {
		t.Errorf("expected LatestID = 2, got %d", stats.LatestID)
	}
	if stats.OldestID != 1 {
		t.Errorf("expected OldestID = 1, got %d", stats.OldestID)
	}

	// Close store
	if err := store.Close(); err != nil {
		t.Fatalf("failed to close store: %v", err)
	}

	// Reopen store from same directory and verify recovery
	reopenedStore, err := OpenStreamStore(tempDir)
	if err != nil {
		t.Fatalf("failed to reopen stream store: %v", err)
	}
	defer reopenedStore.Close()

	reopenedStats := reopenedStore.Stats()
	if reopenedStats.LatestID != 2 {
		t.Errorf("expected recovered LatestID = 2, got %d", reopenedStats.LatestID)
	}
	if reopenedStats.OldestID != 1 {
		t.Errorf("expected recovered OldestID = 1, got %d", reopenedStats.OldestID)
	}

	recoveredMsg, ok, err := reopenedStore.Read(1)
	if err != nil || !ok {
		t.Fatalf("failed to read id 1 from recovered store: ok=%v, err=%v", ok, err)
	}
	if !bytes.Equal(recoveredMsg.Payload, payload1) {
		t.Errorf("recovered payload mismatch: expected %s, got %s", payload1, recoveredMsg.Payload)
	}
}

func TestStreamStoreCloseUnblocks(t *testing.T) {
	tempDir, err := os.MkdirTemp("", "photon-store-close-test-*")
	if err != nil {
		t.Fatalf("failed to create temp dir: %v", err)
	}
	defer os.RemoveAll(tempDir)

	store, err := OpenStreamStore(tempDir)
	if err != nil {
		t.Fatalf("failed to open stream store: %v", err)
	}

	done := make(chan error, 1)
	go func() {
		// Waiting for message 100 which doesn't exist
		_, err := store.ReadOrWait(100)
		done <- err
	}()

	// Close store while ReadOrWait is waiting
	if err := store.Close(); err != nil {
		t.Fatalf("failed to close store: %v", err)
	}

	err = <-done
	if err == nil {
		t.Errorf("expected error from ReadOrWait after store closed, got nil")
	}

	// Append after close should fail
	_, err = store.Append([]byte("should fail"))
	if err == nil {
		t.Errorf("expected error appending to closed store, got nil")
	}
}
