package main

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"net"
	"os"
	"server/cache"
	"sync"
	"testing"
	"time"
)

func TestEndToEndStreamPipeline(t *testing.T) {
	tempDir, err := os.MkdirTemp("", "photon-e2e-test-*")
	if err != nil {
		t.Fatalf("failed to create temp dir: %v", err)
	}
	defer os.RemoveAll(tempDir)

	store, err := OpenStreamStore(tempDir)
	if err != nil {
		t.Fatalf("failed to open stream store: %v", err)
	}
	defer store.Close()

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	// 1. Start Ingress listener on random available port
	ingressListener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("failed to bind ingress listener: %v", err)
	}
	defer ingressListener.Close()

	ingressChan := make(chan []byte, 100)
	var ingressWg sync.WaitGroup
	go listenForIngress(ctx, ingressListener, ingressChan, &ingressWg)

	// Append loop
	go appendLoop(store, ingressChan, cache.New())

	// 2. Start Client fan-out listener on random available port
	clientListener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("failed to bind client listener: %v", err)
	}
	defer clientListener.Close()

	clients := NewClientTracker()
	var clientWg sync.WaitGroup
	gate := NewClientGate("testpw")
	go listenForClients(ctx, clientListener, store, clients, gate, &clientWg)

	// 3. Start Observer telemetry listener on random available port
	observerListener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("failed to bind observer listener: %v", err)
	}
	defer observerListener.Close()

	var observerWg sync.WaitGroup
	videoStore, err := OpenVideoStore(tempDir)
	if err != nil {
		t.Fatalf("failed to open video store: %v", err)
	}
	defer videoStore.Close()

	go listenForObservers(ctx, observerListener, time.Now(), store, clients, ingressChan, videoStore, gate, &observerWg)

	// 4. Connect a downstream client
	clientConn, err := net.Dial("tcp", clientListener.Addr().String())
	if err != nil {
		t.Fatalf("failed to connect downstream client: %v", err)
	}
	defer clientConn.Close()
	if err := WriteClientAuth(clientConn, "testpw"); err != nil {
		t.Fatalf("client auth: %v", err)
	}
	// A client starts at the next message, so one stored before the server has
	// registered it never reaches it. Wait for the registration, not the dial.
	registered := time.Now().Add(5 * time.Second)
	for clients.Count() == 0 {
		if time.Now().After(registered) {
			t.Fatalf("downstream client was not registered within 5s")
		}
		time.Sleep(5 * time.Millisecond)
	}

	// 5. Connect an ingress sender and send test packets
	ingressConn, err := net.Dial("tcp", ingressListener.Addr().String())
	if err != nil {
		t.Fatalf("failed to connect ingress sender: %v", err)
	}
	defer ingressConn.Close()

	testPayload := []byte("CAN_PAYLOAD_TEST_DATA_1234")
	if _, err := ingressConn.Write(testPayload); err != nil {
		t.Fatalf("failed to write ingress packet: %v", err)
	}

	// 6. Read received packet on downstream client
	recvBuf := make([]byte, len(testPayload))
	// Fail here, not at the 10-minute test timeout, if the payload never arrives.
	_ = clientConn.SetReadDeadline(time.Now().Add(10 * time.Second))
	if _, err := io.ReadFull(clientConn, recvBuf); err != nil {
		t.Fatalf("failed to read payload on downstream client: %v", err)
	}

	if !bytes.Equal(recvBuf, testPayload) {
		t.Fatalf("payload mismatch: expected %q, got %q", string(testPayload), string(recvBuf))
	}

	// 7. Verify observer returns valid JSON metadata
	obsConn, err := net.Dial("tcp", observerListener.Addr().String())
	if err != nil {
		t.Fatalf("failed to connect to observer: %v", err)
	}
	defer obsConn.Close()
	if err := WriteClientAuth(obsConn, "testpw"); err != nil {
		t.Fatalf("observer auth: %v", err)
	}

	decoder := json.NewDecoder(obsConn)
	var meta ServerMetadata
	if err := decoder.Decode(&meta); err != nil {
		t.Fatalf("failed to decode observer JSON metadata: %v", err)
	}

	if meta.PID <= 0 {
		t.Errorf("expected positive PID, got %d", meta.PID)
	}
	if !meta.Ingress.Listening {
		t.Errorf("expected Ingress.Listening = true")
	}
	if !meta.Clients.Listening {
		t.Errorf("expected Clients.Listening = true")
	}
	if meta.Video.SegmentCount < 0 {
		t.Errorf("expected non-negative video segment count")
	}

	// 8. Test shutdown
	cancel()
	_ = ingressListener.Close()
	_ = clientListener.Close()
	_ = observerListener.Close()
	_ = store.Close()
}
