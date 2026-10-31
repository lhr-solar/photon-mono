package main

import (
	"bytes"
	"encoding/binary"
	"testing"
)

func TestVideoFrameRoundTrip(t *testing.T) {
	original := VideoFrame{
		TimestampMs: 1_700_000_123_456,
		Flags:       VideoFlagKeyframe | VideoFlagConfig,
		Payload:     []byte{0x00, 0x00, 0x00, 0x01, 0x65, 0x01, 0x02, 0x03},
	}

	var buf bytes.Buffer
	if err := EncodeVideoFrame(&buf, original); err != nil {
		t.Fatalf("encode: %v", err)
	}

	raw := buf.Bytes()
	if len(raw) != videoFrameHeaderBytes+len(original.Payload) {
		t.Fatalf("encoded length %d, want %d", len(raw), videoFrameHeaderBytes+len(original.Payload))
	}
	if got := binary.BigEndian.Uint32(raw[0:4]); got != uint32(len(original.Payload)) {
		t.Fatalf("length prefix %d, want %d", got, len(original.Payload))
	}
	if got := binary.BigEndian.Uint64(raw[4:12]); got != original.TimestampMs {
		t.Fatalf("timestamp %d, want %d", got, original.TimestampMs)
	}
	if raw[12] != original.Flags {
		t.Fatalf("flags 0x%02x, want 0x%02x", raw[12], original.Flags)
	}

	decoded, err := DecodeVideoFrame(&buf)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if decoded.TimestampMs != original.TimestampMs {
		t.Fatalf("timestamp %d, want %d", decoded.TimestampMs, original.TimestampMs)
	}
	if decoded.Flags != original.Flags {
		t.Fatalf("flags 0x%02x, want 0x%02x", decoded.Flags, original.Flags)
	}
	if !bytes.Equal(decoded.Payload, original.Payload) {
		t.Fatalf("payload mismatch:\n got %x\nwant %x", decoded.Payload, original.Payload)
	}
	if !decoded.IsKeyframe() || !decoded.IsConfig() {
		t.Fatal("expected both keyframe and config bits set")
	}
}

func TestVideoFrameEmptyPayload(t *testing.T) {
	var buf bytes.Buffer
	if err := EncodeVideoFrame(&buf, VideoFrame{TimestampMs: 9, Flags: 0}); err != nil {
		t.Fatalf("encode: %v", err)
	}
	decoded, err := DecodeVideoFrame(&buf)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if decoded.TimestampMs != 9 || len(decoded.Payload) != 0 {
		t.Fatalf("unexpected decode: ts=%d len=%d", decoded.TimestampMs, len(decoded.Payload))
	}
}

func TestVideoRequestRoundTrip(t *testing.T) {
	var buf bytes.Buffer
	req := VideoRequest{Mode: VideoModeReplay, StartTimeMs: 42}
	if err := EncodeVideoRequest(&buf, req); err != nil {
		t.Fatalf("encode: %v", err)
	}
	if buf.Len() != videoRequestBytes {
		t.Fatalf("request size %d, want %d", buf.Len(), videoRequestBytes)
	}
	got, err := DecodeVideoRequest(&buf)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if got != req {
		t.Fatalf("got %+v, want %+v", got, req)
	}
}

func TestVideoFlagBits(t *testing.T) {
	if VideoFlagKeyframe != 1<<0 {
		t.Fatalf("keyframe flag must be bit0, got %d", VideoFlagKeyframe)
	}
	if VideoFlagConfig != 1<<1 {
		t.Fatalf("config flag must be bit1, got %d", VideoFlagConfig)
	}
}
