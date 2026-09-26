package main

import (
	"encoding/binary"
	"errors"
	"io"
)

const (
	// VideoFlagKeyframe marks an IDR NAL (bit 0).
	VideoFlagKeyframe uint8 = 1 << 0
	// VideoFlagConfig marks an SPS or PPS NAL (bit 1).
	VideoFlagConfig uint8 = 1 << 1

	VideoModeLive   uint8 = 0
	VideoModeReplay uint8 = 1

	videoFrameHeaderBytes   = 13 // uint32 len + uint64 ts + uint8 flags
	videoRequestBytes       = 9  // uint8 mode + uint64 startMs
	maxVideoFramePayload    = maxNALBytes
	videoWriteTimeoutMillis = 250
	// Catch up a long camera GOP (2–8 s at 30 fps) without falling back to
	// the same IDR. The old 45-NAL cap rewound every real-camera GOP.
	videoLiveMaxLag = 240
	// Live subscribe may seed one recent IDR. Older keyframes are leftover
	// synth or a previous session and must not start the live tail.
	videoLiveKeyframeFreshMs = 2500
)

// VideoFrame is one framed NAL on the video client protocol.
//
// Wire format (big-endian), documented in README:
//
//	uint32  payloadLen     length of the NAL payload only
//	uint64  timestampMs    wall-clock unix milliseconds
//	uint8   flags          bit0 = keyframe (IDR), bit1 = config (SPS/PPS)
//	[payloadLen] payload   Annex-B NAL including 00 00 00 01 start code
type VideoFrame struct {
	TimestampMs uint64
	Flags       uint8
	Payload     []byte
}

func (f VideoFrame) IsKeyframe() bool { return f.Flags&VideoFlagKeyframe != 0 }
func (f VideoFrame) IsConfig() bool   { return f.Flags&VideoFlagConfig != 0 }

func EncodeVideoFrame(w io.Writer, f VideoFrame) error {
	if len(f.Payload) > maxVideoFramePayload {
		return errors.New("video frame payload exceeds maximum size")
	}
	var hdr [videoFrameHeaderBytes]byte
	binary.BigEndian.PutUint32(hdr[0:4], uint32(len(f.Payload)))
	binary.BigEndian.PutUint64(hdr[4:12], f.TimestampMs)
	hdr[12] = f.Flags
	if _, err := w.Write(hdr[:]); err != nil {
		return err
	}
	if len(f.Payload) == 0 {
		return nil
	}
	_, err := w.Write(f.Payload)
	return err
}

func DecodeVideoFrame(r io.Reader) (VideoFrame, error) {
	var hdr [videoFrameHeaderBytes]byte
	if _, err := io.ReadFull(r, hdr[:]); err != nil {
		return VideoFrame{}, err
	}
	payloadLen := binary.BigEndian.Uint32(hdr[0:4])
	if payloadLen > maxVideoFramePayload {
		return VideoFrame{}, errors.New("video frame payload exceeds maximum size")
	}
	payload := make([]byte, payloadLen)
	if payloadLen > 0 {
		if _, err := io.ReadFull(r, payload); err != nil {
			return VideoFrame{}, err
		}
	}
	return VideoFrame{
		TimestampMs: binary.BigEndian.Uint64(hdr[4:12]),
		Flags:       hdr[12],
		Payload:     payload,
	}, nil
}

// VideoRequest is the 9-byte subscribe header a client writes immediately after connect.
//
//	uint8   mode          0 = live, 1 = replay
//	uint64  startTimeMs   replay start (unix ms); ignored for live
type VideoRequest struct {
	Mode        uint8
	StartTimeMs uint64
}

func EncodeVideoRequest(w io.Writer, req VideoRequest) error {
	var buf [videoRequestBytes]byte
	buf[0] = req.Mode
	binary.BigEndian.PutUint64(buf[1:9], req.StartTimeMs)
	_, err := w.Write(buf[:])
	return err
}

func DecodeVideoRequest(r io.Reader) (VideoRequest, error) {
	var buf [videoRequestBytes]byte
	if _, err := io.ReadFull(r, buf[:]); err != nil {
		return VideoRequest{}, err
	}
	return VideoRequest{
		Mode:        buf[0],
		StartTimeMs: binary.BigEndian.Uint64(buf[1:9]),
	}, nil
}
