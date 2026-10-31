package main

import (
	"bytes"
	"context"
	"net"
	"testing"
	"time"
)

func TestEncodeDecodeVideoUdpRoundtrip(t *testing.T) {
	frame := VideoFrame{
		TimestampMs: 42,
		Flags:       VideoFlagKeyframe,
		Payload:     bytes.Repeat([]byte{0, 0, 0, 1, 0x65}, 2500),
	}
	pkts := EncodeVideoUdpPackets(7, frame)
	if len(pkts) < 3 {
		t.Fatalf("want fragments for 2500 B NAL, got %d", len(pkts))
	}
	var asm videoUdpAssembler
	var got VideoFrame
	var ok bool
	for _, raw := range pkts {
		p, err := DecodeVideoUdpPacket(raw)
		if err != nil {
			t.Fatal(err)
		}
		got, ok = asm.add(p)
	}
	if !ok {
		t.Fatal("assembler did not complete")
	}
	if got.TimestampMs != frame.TimestampMs || got.Flags != frame.Flags {
		t.Fatalf("header mismatch ts=%d flags=%d", got.TimestampMs, got.Flags)
	}
	if !bytes.Equal(got.Payload, frame.Payload) {
		t.Fatalf("payload mismatch %d vs %d", len(got.Payload), len(frame.Payload))
	}
}

func TestVideoUdpSubscribeAuth(t *testing.T) {
	server, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 0})
	if err != nil {
		t.Fatal(err)
	}
	defer server.Close()

	dir := t.TempDir()
	store, err := OpenVideoStore(dir)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	base := videoNowMS()
	mustAppend(t, store, annexBNAL(nalTypeSPS, 0x42, 0x00, 0x0a), base)
	mustAppend(t, store, annexBNAL(nalTypePPS, 0xce, 0x38), base)
	mustAppend(t, store, annexBNAL(nalTypeIDR, 0x01, 0x77), base)

	fan := NewVideoUdpFanout(server, store, "secret")
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go fan.Run(ctx)

	client, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 0})
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()

	bad, err := EncodeVideoUdpSubscribe("nope", true)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := client.WriteTo(bad, server.LocalAddr()); err != nil {
		t.Fatal(err)
	}
	time.Sleep(50 * time.Millisecond)
	if n := fan.SubCount(); n != 0 {
		t.Fatalf("bad password subscribed n=%d", n)
	}

	okPkt, err := EncodeVideoUdpSubscribe("secret", true)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := client.WriteTo(okPkt, server.LocalAddr()); err != nil {
		t.Fatal(err)
	}
	deadline := time.Now().Add(500 * time.Millisecond)
	for time.Now().Before(deadline) && fan.SubCount() == 0 {
		time.Sleep(10 * time.Millisecond)
	}
	if fan.SubCount() != 1 {
		t.Fatalf("subscribe n=%d", fan.SubCount())
	}

	_ = client.SetReadDeadline(time.Now().Add(400 * time.Millisecond))
	buf := make([]byte, 2048)
	n, _, err := client.ReadFrom(buf)
	if err != nil {
		t.Fatalf("expected seed datagram: %v", err)
	}
	p, err := DecodeVideoUdpPacket(buf[:n])
	if err != nil {
		t.Fatal(err)
	}
	if p.Flags&VideoFlagConfig == 0 && p.Flags&VideoFlagKeyframe == 0 {
		t.Fatalf("seed flags 0x%02x want config or key", p.Flags)
	}

	unsub, err := EncodeVideoUdpSubscribe("secret", false)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := client.WriteTo(unsub, server.LocalAddr()); err != nil {
		t.Fatal(err)
	}
	deadline = time.Now().Add(400 * time.Millisecond)
	for time.Now().Before(deadline) && fan.SubCount() != 0 {
		time.Sleep(10 * time.Millisecond)
	}
	if fan.SubCount() != 0 {
		t.Fatalf("unsubscribe left n=%d", fan.SubCount())
	}
}

func TestVideoUdpFanoutLiveNAL(t *testing.T) {
	server, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 0})
	if err != nil {
		t.Fatal(err)
	}
	defer server.Close()
	dir := t.TempDir()
	store, err := OpenVideoStore(dir)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	base := videoNowMS()
	mustAppend(t, store, annexBNAL(nalTypeSPS, 0x42), base)
	mustAppend(t, store, annexBNAL(nalTypeIDR, 0x01), base)

	fan := NewVideoUdpFanout(server, store, "pw")
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go fan.Run(ctx)

	client, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 0})
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	sub, _ := EncodeVideoUdpSubscribe("pw", true)
	if _, err := client.WriteTo(sub, server.LocalAddr()); err != nil {
		t.Fatal(err)
	}
	deadline := time.Now().Add(400 * time.Millisecond)
	for time.Now().Before(deadline) && fan.SubCount() == 0 {
		time.Sleep(5 * time.Millisecond)
	}

	pSlice := annexBNAL(1, 0x22, 0x33, 0x44)
	mustAppend(t, store, pSlice, base+33)

	_ = client.SetReadDeadline(time.Now().Add(800 * time.Millisecond))
	buf := make([]byte, 4096)
	sawP := false
	for i := 0; i < 16; i++ {
		n, _, err := client.ReadFrom(buf)
		if err != nil {
			break
		}
		p, err := DecodeVideoUdpPacket(buf[:n])
		if err != nil {
			continue
		}
		if bytes.Contains(p.Payload, []byte{0x22, 0x33, 0x44}) {
			sawP = true
			break
		}
	}
	if !sawP {
		t.Fatal("did not receive live P-slice on UDP fan-out")
	}
}
