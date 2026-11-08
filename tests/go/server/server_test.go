package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"io"
	"net"
	"os"
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

	pool := newBatchPool(4, 1<<16)
	var ingressWg sync.WaitGroup
	go listenForIngress(ctx, ingressListener, &ingressWg, pool)

	// Append loop; CAN decoding starts off and the observer turns it on below.
	canCfg := DefaultCANConfig()
	canCfg.DBCDir = testDBCDir
	canSettings := newCANSettings(canCfg, nil)
	go appendLoop(store, pool, nil, canSettings.swap)

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

	go listenForObservers(ctx, observerListener, time.Now(), store, clients, pool, videoStore, canSettings, gate, &observerWg)

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

	// Ingress reads whole CANP batches, so send one: a header for one frame,
	// then the 29-byte frame record.
	testPayload := make([]byte, canpHeaderSize+canpFrameSize)
	binary.BigEndian.PutUint32(testPayload[0:4], canpMagic)
	binary.BigEndian.PutUint16(testPayload[4:6], canpVersion)
	binary.BigEndian.PutUint16(testPayload[6:8], 1)
	copy(testPayload[canpHeaderSize:], "CAN_PAYLOAD_TEST_DATA_1234")
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
	if meta.Config.CAN.Decode || meta.Config.CAN.DBCDir != testDBCDir || len(meta.Config.CAN.SampleRates) == 0 {
		t.Errorf("config = %+v, want decoding off with the test DBC dir and sample rates", meta.Config.CAN)
	}

	// 8. The observer changes settings on the same connection.
	send := func(cmd string) replyBody {
		t.Helper()
		if _, err := obsConn.Write([]byte(cmd + "\n")); err != nil {
			t.Fatalf("send %s: %v", cmd, err)
		}
		_ = obsConn.SetReadDeadline(time.Now().Add(10 * time.Second))
		for { // metadata lines keep coming; skip to the reply
			var line struct {
				Reply *replyBody `json:"reply"`
			}
			if err := decoder.Decode(&line); err != nil {
				t.Fatalf("waiting for reply to %s: %v", cmd, err)
			}
			if line.Reply != nil {
				return *line.Reply
			}
		}
	}
	if r := send(`{"set":{"can":{"dbc_dir":"no/such/dir","decode":true}}}`); r.OK || r.Error == "" {
		t.Errorf("a missing DBC dir was accepted: %+v", r)
	}
	if r := send(`{"set":{"can":{"decode":true}}}`); !r.OK {
		t.Fatalf("turning decoding on failed: %s", r.Error)
	}
	if err := decoder.Decode(&meta); err != nil { // sent right after the reply
		t.Fatal(err)
	}
	if !meta.Config.CAN.Decode || !meta.CAN.Enabled {
		t.Errorf("after turning decoding on: config %+v, can %+v", meta.Config.CAN, meta.CAN)
	}
	if r := send(`{"set":{}}`); r.OK {
		t.Errorf("an empty command was accepted")
	}

	// 9. Test shutdown
	cancel()
	_ = ingressListener.Close()
	_ = clientListener.Close()
	_ = observerListener.Close()
	_ = store.Close()
}
