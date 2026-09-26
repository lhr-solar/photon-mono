package main

import (
	"encoding/binary"
)

const (
	rtpVersion           = 2
	rtpPayloadTypeH264   = 96
	rtpHeaderMinBytes    = 12
	nalTypeSingleMin     = 1
	nalTypeSingleMax     = 23
	nalTypeSTAPA         = 24
	nalTypeFUA           = 28
	nalTypeIDR           = 5
	nalTypeSEI           = 6
	nalTypeSPS           = 7
	nalTypePPS           = 8
	nalTypeAUD           = 9
	nalTypeEOSeq         = 10
	nalTypeEOStream      = 11
	nalTypeFiller        = 12
	maxNALBytes          = 16 * 1024 * 1024
	annexBStartCodeBytes = 4
)

var annexBStartCode = []byte{0x00, 0x00, 0x00, 0x01}

// NALUnit is a depacketized H.264 NAL with an Annex-B start code.
type NALUnit struct {
	Payload      []byte
	RTPTimestamp uint32
	Sequence     uint16
	NALType      uint8
	Keyframe     bool
	Config       bool
}

// RTPDepacketizer reassembles RTP/H.264 packets into Annex-B NAL units.
// Single-NAL packets and FU-A fragments are required; STAP-A is accepted.
// Sequence gaps and late/out-of-order packets drop any in-progress FU-A
// and otherwise continue — no reorder buffer is kept.
type RTPDepacketizer struct {
	expectedSeq uint16
	seqStarted  bool
	inFU        bool
	fuBuf       []byte
	fuType      uint8
}

// Push consumes one UDP datagram. It returns zero or more complete NALs.
// Incomplete fragments are held internally until an FU-A end bit arrives.
func (d *RTPDepacketizer) Push(datagram []byte) []NALUnit {
	payload, seq, ts, ok := parseRTPPayload(datagram)
	if !ok {
		return nil
	}

	if d.seqStarted {
		delta := int16(seq - d.expectedSeq)
		if delta != 0 {
			d.resetFragment()
			if delta < 0 {
				// Late / out-of-order: drop this packet, keep expecting the next in-order seq.
				return nil
			}
			// Forward gap: accept this packet as the new timeline.
		}
	}
	d.expectedSeq = seq + 1
	d.seqStarted = true

	if len(payload) == 0 {
		return nil
	}

	nalType := payload[0] & 0x1f
	switch {
	case nalType >= nalTypeSingleMin && nalType <= nalTypeSingleMax:
		d.resetFragment()
		return []NALUnit{makeNAL(payload, ts, seq)}
	case nalType == nalTypeSTAPA:
		d.resetFragment()
		return splitSTAPA(payload, ts, seq)
	case nalType == nalTypeFUA:
		return d.pushFUA(payload, ts, seq)
	default:
		d.resetFragment()
		return nil
	}
}

func (d *RTPDepacketizer) pushFUA(payload []byte, ts uint32, seq uint16) []NALUnit {
	if len(payload) < 2 {
		d.resetFragment()
		return nil
	}
	start := payload[1]&0x80 != 0
	end := payload[1]&0x40 != 0
	fragType := payload[1] & 0x1f
	frag := payload[2:]

	if start {
		if d.inFU {
			d.resetFragment()
		}
		header := (payload[0] & 0xe0) | fragType
		d.fuBuf = make([]byte, 0, 1+len(frag))
		d.fuBuf = append(d.fuBuf, header)
		d.fuBuf = append(d.fuBuf, frag...)
		d.fuType = fragType
		d.inFU = true
	} else if !d.inFU {
		return nil
	} else {
		if uint64(len(d.fuBuf))+uint64(len(frag)) > maxNALBytes {
			d.resetFragment()
			return nil
		}
		d.fuBuf = append(d.fuBuf, frag...)
	}

	if !d.inFU || !end {
		return nil
	}
	nal := makeNAL(d.fuBuf, ts, seq)
	d.resetFragment()
	return []NALUnit{nal}
}

func (d *RTPDepacketizer) resetFragment() {
	d.inFU = false
	d.fuBuf = nil
	d.fuType = 0
}

func parseRTPPayload(datagram []byte) (payload []byte, seq uint16, ts uint32, ok bool) {
	if len(datagram) < rtpHeaderMinBytes {
		return nil, 0, 0, false
	}
	if datagram[0]>>6 != rtpVersion {
		return nil, 0, 0, false
	}
	if datagram[1]&0x7f != rtpPayloadTypeH264 {
		return nil, 0, 0, false
	}

	cc := int(datagram[0] & 0x0f)
	headerSize := rtpHeaderMinBytes + cc*4
	if headerSize > len(datagram) {
		return nil, 0, 0, false
	}
	if datagram[0]&0x10 != 0 {
		if headerSize+4 > len(datagram) {
			return nil, 0, 0, false
		}
		extWords := int(binary.BigEndian.Uint16(datagram[headerSize+2 : headerSize+4]))
		headerSize += 4 + extWords*4
		if headerSize > len(datagram) {
			return nil, 0, 0, false
		}
	}

	payloadEnd := len(datagram)
	if datagram[0]&0x20 != 0 {
		pad := int(datagram[payloadEnd-1])
		if pad == 0 || pad > payloadEnd-headerSize {
			return nil, 0, 0, false
		}
		payloadEnd -= pad
	}
	if headerSize >= payloadEnd {
		return nil, 0, 0, false
	}

	seq = binary.BigEndian.Uint16(datagram[2:4])
	ts = binary.BigEndian.Uint32(datagram[4:8])
	return datagram[headerSize:payloadEnd], seq, ts, true
}

func splitSTAPA(payload []byte, ts uint32, seq uint16) []NALUnit {
	var out []NALUnit
	offset := 1
	for offset+2 <= len(payload) {
		nalSize := int(binary.BigEndian.Uint16(payload[offset : offset+2]))
		offset += 2
		if nalSize == 0 || offset+nalSize > len(payload) {
			break
		}
		out = append(out, makeNAL(payload[offset:offset+nalSize], ts, seq))
		offset += nalSize
	}
	return out
}

func makeNAL(raw []byte, ts uint32, seq uint16) NALUnit {
	payload := make([]byte, annexBStartCodeBytes+len(raw))
	copy(payload, annexBStartCode)
	copy(payload[annexBStartCodeBytes:], raw)
	nalType := uint8(0)
	if len(raw) > 0 {
		nalType = raw[0] & 0x1f
	}
	return NALUnit{
		Payload:      payload,
		RTPTimestamp: ts,
		Sequence:     seq,
		NALType:      nalType,
		Keyframe:     nalType == nalTypeIDR,
		Config:       nalType == nalTypeSPS || nalType == nalTypePPS,
	}
}

func annexBStartLen(data []byte) int {
	if len(data) >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1 {
		return 4
	}
	if len(data) >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1 {
		return 3
	}
	return 0
}

func nalTypeOfAnnexB(data []byte) uint8 {
	n := annexBStartLen(data)
	if n == 0 || n >= len(data) {
		return 0
	}
	return data[n] & 0x1f
}

func discardableNALType(nalType uint8) bool {
	return nalType == nalTypeSEI || nalType == nalTypeAUD || nalType == nalTypeEOSeq ||
		nalType == nalTypeEOStream || nalType == nalTypeFiller
}

func flagsForNALType(nalType uint8) uint8 {
	var flags uint8
	if nalType == nalTypeIDR {
		flags |= VideoFlagKeyframe
	}
	if nalType == nalTypeSPS || nalType == nalTypePPS {
		flags |= VideoFlagConfig
	}
	return flags
}

func splitAnnexB(data []byte) [][]byte {
	if len(data) == 0 {
		return nil
	}
	starts := make([]int, 0, 8)
	for i := 0; i+3 <= len(data); {
		if i+4 <= len(data) && data[i] == 0 && data[i+1] == 0 && data[i+2] == 0 && data[i+3] == 1 {
			starts = append(starts, i)
			i += 4
			continue
		}
		if data[i] == 0 && data[i+1] == 0 && data[i+2] == 1 {
			starts = append(starts, i)
			i += 3
			continue
		}
		i++
	}
	if len(starts) == 0 {
		return nil
	}
	out := make([][]byte, 0, len(starts))
	for i, start := range starts {
		end := len(data)
		if i+1 < len(starts) {
			end = starts[i+1]
		}
		nal := data[start:end]
		if annexBStartLen(nal) == 3 {
			rewritten := make([]byte, 0, 1+len(nal))
			rewritten = append(rewritten, annexBStartCode...)
			rewritten = append(rewritten, nal[3:]...)
			nal = rewritten
		} else {
			cp := make([]byte, len(nal))
			copy(cp, nal)
			nal = cp
		}
		if len(nal) > annexBStartCodeBytes {
			out = append(out, nal)
		}
	}
	return out
}
