package main

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"log"
	"net"
	"os"
	"path/filepath"
	"sync"
	"time"
)

// RTP/RTCP clock alignment (tsdb-architecture.md §8.5).
//
// Telemetry timestamps come from the car's CANP batch header. Video timestamps
// used to come from the server's receive wall clock, which looks comparable and
// is not: it differs by network delay plus whatever the two clocks disagree by.
//
// The fix is to carry the RTP timestamp and map it to wall clock through RTCP
// sender reports, which state "RTP timestamp X was NTP time Y" at the sender.
// Receive time survives only as a labelled fallback.
const (
	rtcpPayloadTypeSR = 200
	rtcpHeaderBytes   = 8
	rtcpSRBodyBytes   = 20
	// H.264 RTP streams are 90 kHz by RFC 6184.
	rtpClockRateH264 = uint32(90000)
	// Seconds between the NTP epoch (1900) and the unix epoch (1970).
	ntpUnixEpochDelta = uint64(2208988800)
	// A sender report older than this is not trusted to map current
	// timestamps; senders emit one every ~5 s, so a minute of silence means
	// the sender is gone or its clock story has changed.
	rtcpStaleMs = uint64(60_000)
)

// videoClockFile is the persisted manual nudge and last known provenance
// (§8.5 items 2 and 3). It is a manifest, not a cache: the manual offset is the
// only thing that works when the camera has no usable clock, so it has to
// survive a restart.
type videoClockFile struct {
	OffsetMs   int64  `json:"video_clock_offset_ms"`
	Provenance string `json:"provenance"`
	UpdatedMs  uint64 `json:"updated_ms"`
}

// RTPClock maps RTP timestamps to unix wall-clock milliseconds.
type RTPClock struct {
	mu sync.Mutex

	haveSR    bool
	srNtpMs   uint64
	srRtpTs   uint32
	srSeenMs  uint64
	clockRate uint32

	// Manual ±ms nudge, applied on top of whatever mapping is in force and
	// reported to clients so they can apply it to already-stored timestamps.
	manualOffsetMs int64

	path string
}

func NewRTPClock() *RTPClock {
	return &RTPClock{clockRate: rtpClockRateH264}
}

// Load reads the persisted manual offset. A missing file is not an error.
func (c *RTPClock) Load(dataDir string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.path = filepath.Join(dataDir, "video", "video_clock.json")
	data, err := os.ReadFile(c.path)
	if err != nil {
		return
	}
	var f videoClockFile
	if err := json.Unmarshal(data, &f); err != nil {
		log.Printf("[photon-server] video_clock.json is unreadable: %v", err)
		return
	}
	c.manualOffsetMs = f.OffsetMs
}

// SetManualOffset records a manual nudge and persists it.
func (c *RTPClock) SetManualOffset(offsetMs int64) error {
	c.mu.Lock()
	c.manualOffsetMs = offsetMs
	path := c.path
	prov := c.provenanceLocked(videoNowMS())
	c.mu.Unlock()
	if path == "" {
		return nil
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	data, err := json.MarshalIndent(videoClockFile{
		OffsetMs:   offsetMs,
		Provenance: clockProvenanceName(prov),
		UpdatedMs:  videoNowMS(),
	}, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(path, data, 0o644)
}

func (c *RTPClock) ManualOffset() int64 {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.manualOffsetMs
}

// ApplySR folds one sender report into the mapping.
func (c *RTPClock) ApplySR(ntpMs uint64, rtpTs uint32, nowMs uint64) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.haveSR = true
	c.srNtpMs = ntpMs
	c.srRtpTs = rtpTs
	c.srSeenMs = nowMs
}

// WallMs maps an RTP timestamp to unix ms, or falls back to receive time.
//
// The RTP timestamp is 32 bits and wraps roughly every 13 h at 90 kHz, so the
// difference against the sender report is taken as a signed 32-bit quantity;
// naive unsigned subtraction turns a small negative delta into ~13 h.
func (c *RTPClock) WallMs(rtpTs uint32, receiveMs uint64) (uint64, uint8) {
	c.mu.Lock()
	defer c.mu.Unlock()
	apply := func(base uint64, prov uint8) (uint64, uint8) {
		adj := int64(base) + c.manualOffsetMs
		if adj < 0 {
			return 0, prov
		}
		return uint64(adj), prov
	}
	if !c.haveSR {
		return apply(receiveMs, clockProvReceive)
	}
	if receiveMs > c.srSeenMs && receiveMs-c.srSeenMs > rtcpStaleMs {
		return apply(receiveMs, clockProvReceive)
	}
	rate := c.clockRate
	if rate == 0 {
		rate = rtpClockRateH264
	}
	deltaTicks := int64(int32(rtpTs - c.srRtpTs))
	deltaMs := deltaTicks * 1000 / int64(rate)
	mapped := int64(c.srNtpMs) + deltaMs
	if mapped < 0 {
		return apply(receiveMs, clockProvReceive)
	}
	return apply(uint64(mapped), clockProvRTCP)
}

// Provenance is what a client must check before believing an A/V sync claim.
// Anything below clockProvRTCP means the offset is unverified (§8.5).
func (c *RTPClock) Provenance() uint8 {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.provenanceLocked(videoNowMS())
}

func (c *RTPClock) provenanceLocked(nowMs uint64) uint8 {
	if c.haveSR && !(nowMs > c.srSeenMs && nowMs-c.srSeenMs > rtcpStaleMs) {
		return clockProvRTCP
	}
	if c.manualOffsetMs != 0 {
		return clockProvManual
	}
	return clockProvReceive
}

// ntpToUnixMs converts a 64-bit NTP timestamp to unix milliseconds.
func ntpToUnixMs(msw uint32, lsw uint32) uint64 {
	if uint64(msw) < ntpUnixEpochDelta {
		return 0
	}
	secs := uint64(msw) - ntpUnixEpochDelta
	// Round to nearest millisecond. Truncating loses up to a full millisecond,
	// which is enough to make a sender report disagree with itself by 1 ms and
	// shows up directly in the A/V offset this mapping exists to establish.
	frac := (uint64(lsw)*1000 + (1 << 31)) >> 32
	return secs*1000 + frac
}

// parseRTCPSenderReports walks a compound RTCP packet and returns the
// (ntpMs, rtpTimestamp) pair from every sender report in it.
func parseRTCPSenderReports(datagram []byte) [](struct {
	NtpMs uint64
	RtpTs uint32
}) {
	type pair = struct {
		NtpMs uint64
		RtpTs uint32
	}
	var out []pair
	off := 0
	for off+rtcpHeaderBytes <= len(datagram) {
		if datagram[off]>>6 != rtpVersion {
			return out
		}
		pt := datagram[off+1]
		lengthWords := int(binary.BigEndian.Uint16(datagram[off+2 : off+4]))
		total := (lengthWords + 1) * 4
		if total <= 0 || off+total > len(datagram) {
			return out
		}
		if pt == rtcpPayloadTypeSR && off+rtcpHeaderBytes+rtcpSRBodyBytes <= len(datagram) {
			body := datagram[off+8:]
			msw := binary.BigEndian.Uint32(body[0:4])
			lsw := binary.BigEndian.Uint32(body[4:8])
			rtpTs := binary.BigEndian.Uint32(body[8:12])
			if ms := ntpToUnixMs(msw, lsw); ms != 0 {
				out = append(out, pair{NtpMs: ms, RtpTs: rtpTs})
			}
		}
		off += total
	}
	return out
}

// listenRTCP feeds sender reports into the clock. RTCP conventionally sits on
// the RTP port plus one.
func listenRTCP(ctx context.Context, conn net.PacketConn, clock *RTPClock) {
	defer conn.Close()
	buf := make([]byte, 4096)
	reported := false
	for {
		select {
		case <-ctx.Done():
			return
		default:
		}
		_ = conn.SetReadDeadline(time.Now().Add(500 * time.Millisecond))
		n, _, err := conn.ReadFrom(buf)
		if err != nil {
			if ne, ok := err.(net.Error); ok && ne.Timeout() {
				continue
			}
			select {
			case <-ctx.Done():
				return
			default:
			}
			continue
		}
		for _, sr := range parseRTCPSenderReports(buf[:n]) {
			clock.ApplySR(sr.NtpMs, sr.RtpTs, videoNowMS())
			if !reported {
				reported = true
				log.Printf("[photon-server] RTCP sender report: rtp=%d maps to unix %d ms; "+
					"video timestamps are now rtcp-aligned", sr.RtpTs, sr.NtpMs)
			}
		}
	}
}
