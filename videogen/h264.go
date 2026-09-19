// Package videogen builds a small, genuinely decodable H.264 Annex-B stream and
// packetizes it as RTP.
//
// tsdb-architecture.md §8 states there is no recorded video corpus yet, so the
// writer path has to be validated against video generated here. "Decodable" is
// the whole point: bytes that merely look like H.264 would exercise the index
// while proving nothing about whether a client could ever start a decoder at a
// pointer this index hands out.
//
// The trick that keeps this small is picking the two macroblock types that need
// no transform, no prediction and no entropy coding beyond exp-Golomb:
//
//   - I_PCM (mb_type 25 in an I slice) stores raw samples, so an IDR frame is a
//     slice header plus 384 bytes per macroblock.
//   - P_Skip (via mb_skip_run covering every macroblock) makes a P frame that
//     repeats its reference, so a GOP has real IDR + P structure rather than
//     being all keyframes.
//
// The result is baseline profile, CAVLC, frame_mbs_only, pic_order_cnt_type 2.
package videogen

import (
	"encoding/binary"
)

// ---------------------------------------------------------------------------
// bit writer
// ---------------------------------------------------------------------------

type bitWriter struct {
	buf  []byte
	cur  byte
	nBit uint8
}

func (w *bitWriter) writeBit(b uint32) {
	w.cur <<= 1
	if b&1 != 0 {
		w.cur |= 1
	}
	w.nBit++
	if w.nBit == 8 {
		w.buf = append(w.buf, w.cur)
		w.cur = 0
		w.nBit = 0
	}
}

func (w *bitWriter) writeBits(v uint32, n uint8) {
	for i := int(n) - 1; i >= 0; i-- {
		w.writeBit(v >> uint(i))
	}
}

func (w *bitWriter) byteAligned() bool { return w.nBit == 0 }

// writeUE emits an unsigned exp-Golomb code, ue(v) in the spec's notation.
func (w *bitWriter) writeUE(v uint32) {
	// codeNum v is written as leadingZeros zero bits, a 1, then the remainder.
	val := uint64(v) + 1
	bits := uint8(0)
	for t := val; t > 0; t >>= 1 {
		bits++
	}
	for i := uint8(0); i < bits-1; i++ {
		w.writeBit(0)
	}
	for i := int(bits) - 1; i >= 0; i-- {
		w.writeBit(uint32(val >> uint(i)))
	}
}

// writeSE emits a signed exp-Golomb code, se(v).
func (w *bitWriter) writeSE(v int32) {
	if v <= 0 {
		w.writeUE(uint32(-2 * v))
		return
	}
	w.writeUE(uint32(2*v - 1))
}

// trailing writes rbsp_trailing_bits(): a stop bit then zeroes to a byte edge.
func (w *bitWriter) trailing() {
	w.writeBit(1)
	for !w.byteAligned() {
		w.writeBit(0)
	}
}

func (w *bitWriter) bytes() []byte {
	if w.nBit != 0 {
		// Only reachable if a caller forgets rbsp_trailing_bits; pad so the
		// result is at least well-formed bytes.
		out := append([]byte(nil), w.buf...)
		return append(out, w.cur<<(8-w.nBit))
	}
	return append([]byte(nil), w.buf...)
}

// ---------------------------------------------------------------------------
// RBSP -> EBSP
// ---------------------------------------------------------------------------

// escapeRBSP inserts emulation prevention bytes. I_PCM samples are raw bytes in
// the RBSP, so a run of zeroes in the picture would otherwise be read as a start
// code and truncate the NAL.
func escapeRBSP(rbsp []byte) []byte {
	out := make([]byte, 0, len(rbsp)+len(rbsp)/64+8)
	zeros := 0
	for _, b := range rbsp {
		if zeros >= 2 && b <= 0x03 {
			out = append(out, 0x03)
			zeros = 0
		}
		out = append(out, b)
		if b == 0 {
			zeros++
		} else {
			zeros = 0
		}
	}
	return out
}

func nalUnit(nalRefIdc uint8, nalType uint8, rbsp []byte) []byte {
	out := make([]byte, 0, 5+len(rbsp))
	out = append(out, 0x00, 0x00, 0x00, 0x01)
	out = append(out, (nalRefIdc<<5)|(nalType&0x1f))
	out = append(out, escapeRBSP(rbsp)...)
	return out
}

// ---------------------------------------------------------------------------
// parameter sets
// ---------------------------------------------------------------------------

// Config describes the stream to generate.
type Config struct {
	// WidthMBs/HeightMBs are in 16x16 macroblocks. 4x3 is 64x48, which makes a
	// ~4.6 kB IDR: big enough to force RTP fragmentation, small enough to keep
	// a whole test corpus in memory.
	WidthMBs  int
	HeightMBs int
	// GopFrames is the IDR interval in frames. §8 requires a 1-2 s IDR
	// interval; at 30 fps, 45 frames is 1.5 s.
	GopFrames int
	Gops      int
	FrameRate int
}

// DefaultConfig is a 64x48, 30 fps stream with a 1.5 s IDR interval.
func DefaultConfig() Config {
	return Config{WidthMBs: 4, HeightMBs: 3, GopFrames: 45, Gops: 4, FrameRate: 30}
}

func (c Config) mbCount() int { return c.WidthMBs * c.HeightMBs }

const (
	log2MaxFrameNumMinus4 = 0 // max_frame_num = 16
	frameNumBits          = uint8(4)
	nalTypeSlice          = uint8(1)
	nalTypeIDR            = uint8(5)
	nalTypeSPS            = uint8(7)
	nalTypePPS            = uint8(8)
	mbTypeIPCM            = uint32(25)
	sliceTypeIAll         = uint32(7)
	sliceTypePAll         = uint32(5)
)

// SPS builds a baseline-profile sequence parameter set NAL.
func (c Config) SPS() []byte {
	w := &bitWriter{}
	w.writeBits(66, 8) // profile_idc: baseline
	w.writeBits(0, 8)  // constraint_setN_flags + reserved_zero_2bits
	w.writeBits(30, 8) // level_idc 3.0
	w.writeUE(0)       // seq_parameter_set_id
	w.writeUE(log2MaxFrameNumMinus4)
	// pic_order_cnt_type 2: display order equals decode order, so the slice
	// header carries no POC syntax at all.
	w.writeUE(2)
	w.writeUE(1)  // max_num_ref_frames
	w.writeBit(0) // gaps_in_frame_num_value_allowed_flag
	w.writeUE(uint32(c.WidthMBs - 1))
	w.writeUE(uint32(c.HeightMBs - 1))
	w.writeBit(1) // frame_mbs_only_flag
	w.writeBit(1) // direct_8x8_inference_flag
	w.writeBit(0) // frame_cropping_flag
	w.writeBit(0) // vui_parameters_present_flag
	w.trailing()
	return nalUnit(3, nalTypeSPS, w.bytes())
}

// PPS builds the matching picture parameter set NAL. CAVLC, no deblocking
// control, no slice groups.
func (c Config) PPS() []byte {
	w := &bitWriter{}
	w.writeUE(0)  // pic_parameter_set_id
	w.writeUE(0)  // seq_parameter_set_id
	w.writeBit(0) // entropy_coding_mode_flag: CAVLC
	w.writeBit(0) // bottom_field_pic_order_in_frame_present_flag
	w.writeUE(0)  // num_slice_groups_minus1
	w.writeUE(0)  // num_ref_idx_l0_default_active_minus1
	w.writeUE(0)  // num_ref_idx_l1_default_active_minus1
	w.writeBit(0) // weighted_pred_flag
	w.writeBits(0, 2)
	w.writeSE(0)  // pic_init_qp_minus26
	w.writeSE(0)  // pic_init_qs_minus26
	w.writeSE(0)  // chroma_qp_index_offset
	w.writeBit(0) // deblocking_filter_control_present_flag
	w.writeBit(0) // constrained_intra_pred_flag
	w.writeBit(0) // redundant_pic_cnt_present_flag
	w.trailing()
	return nalUnit(3, nalTypePPS, w.bytes())
}

// IDRSlice builds an IDR picture whose every macroblock is I_PCM.
//
// frameIndex only steers the sample pattern, so a decoder's output differs per
// GOP and "the right GOP was decoded" is checkable from the pixels.
func (c Config) IDRSlice(frameIndex int) []byte {
	w := &bitWriter{}
	w.writeUE(0)             // first_mb_in_slice
	w.writeUE(sliceTypeIAll) // slice_type: I, all slices
	w.writeUE(0)             // pic_parameter_set_id
	w.writeBits(0, frameNumBits)
	w.writeUE(0)  // idr_pic_id
	w.writeBit(0) // no_output_of_prior_pics_flag
	w.writeBit(0) // long_term_reference_flag
	w.writeSE(0)  // slice_qp_delta

	for mb := 0; mb < c.mbCount(); mb++ {
		w.writeUE(mbTypeIPCM)
		for !w.byteAligned() {
			w.writeBit(0) // pcm_alignment_zero_bit
		}
		luma, chroma := pcmSamples(frameIndex, mb)
		for _, s := range luma {
			w.writeBits(uint32(s), 8)
		}
		for _, s := range chroma {
			w.writeBits(uint32(s), 8)
		}
	}
	w.trailing()
	return nalUnit(3, nalTypeIDR, w.bytes())
}

// PSlice builds a P picture in which every macroblock is skipped, so it repeats
// its reference exactly.
func (c Config) PSlice(frameNum int) []byte {
	w := &bitWriter{}
	w.writeUE(0)             // first_mb_in_slice
	w.writeUE(sliceTypePAll) // slice_type: P, all slices
	w.writeUE(0)             // pic_parameter_set_id
	w.writeBits(uint32(frameNum%16), frameNumBits)
	w.writeBit(0) // num_ref_idx_active_override_flag
	w.writeBit(0) // ref_pic_list_modification_flag_l0
	w.writeBit(0) // adaptive_ref_pic_marking_mode_flag
	w.writeSE(0)  // slice_qp_delta
	// slice_data: one mb_skip_run covering the whole picture. more_rbsp_data()
	// is then false, which is what ends the slice under CAVLC.
	w.writeUE(uint32(c.mbCount()))
	w.trailing()
	return nalUnit(2, nalTypeSlice, w.bytes())
}

// pcmSamples returns the 256 luma and 128 chroma bytes of one macroblock.
//
// Values stay inside 16..235 so the picture is legal video range and a decoder
// that clips will not change them.
func pcmSamples(frameIndex, mb int) ([]byte, []byte) {
	luma := make([]byte, 256)
	base := 16 + ((frameIndex*37 + mb*11) % 200)
	for i := range luma {
		luma[i] = byte(16 + ((base + i) % 200))
	}
	chroma := make([]byte, 128)
	for i := range chroma {
		chroma[i] = byte(16 + ((base + i*3) % 200))
	}
	return luma, chroma
}

// ---------------------------------------------------------------------------
// stream assembly
// ---------------------------------------------------------------------------

// Frame is one generated picture plus where it sits in time.
type Frame struct {
	NALs     [][]byte // Annex-B, SPS/PPS first on a GOP boundary
	Keyframe bool
	// RTPTimestamp advances at 90 kHz, the H.264 RTP clock rate.
	RTPTimestamp uint32
	// PresentationMs is the frame's offset from stream start.
	PresentationMs uint64
}

// Stream generates the whole corpus: Gops GOPs of GopFrames frames each, with
// SPS and PPS repeated ahead of every IDR so a client that starts mid-stream can
// configure a decoder.
func Stream(c Config, rtpStart uint32) []Frame {
	sps := c.SPS()
	pps := c.PPS()
	frames := make([]Frame, 0, c.Gops*c.GopFrames)
	tickPerFrame := uint32(90000 / c.FrameRate)
	idx := 0
	for g := 0; g < c.Gops; g++ {
		for f := 0; f < c.GopFrames; f++ {
			ts := rtpStart + uint32(idx)*tickPerFrame
			ms := uint64(idx) * uint64(1000/c.FrameRate)
			if f == 0 {
				frames = append(frames, Frame{
					NALs:           [][]byte{sps, pps, c.IDRSlice(g)},
					Keyframe:       true,
					RTPTimestamp:   ts,
					PresentationMs: ms,
				})
			} else {
				frames = append(frames, Frame{
					NALs:           [][]byte{c.PSlice(f)},
					RTPTimestamp:   ts,
					PresentationMs: ms,
				})
			}
			idx++
		}
	}
	return frames
}

// AnnexB concatenates a frame list into a single elementary stream, the form a
// decoder or ffmpeg reads directly.
func AnnexB(frames []Frame) []byte {
	var out []byte
	for _, fr := range frames {
		for _, nal := range fr.NALs {
			out = append(out, nal...)
		}
	}
	return out
}

// ---------------------------------------------------------------------------
// RTP packetization
// ---------------------------------------------------------------------------

const (
	rtpPayloadTypeH264 = 96
	// Payload budget per packet. Small enough that a 4.6 kB IDR must be sent
	// as FU-A fragments, which is the path real cameras use.
	rtpMaxPayload = 1200
)

// Packetizer turns NALs into RTP datagrams with monotonic sequence numbers.
type Packetizer struct {
	SSRC uint32
	seq  uint16
}

func (p *Packetizer) header(ts uint32, marker bool) []byte {
	h := make([]byte, 12)
	h[0] = 0x80 // version 2
	h[1] = rtpPayloadTypeH264
	if marker {
		h[1] |= 0x80
	}
	binary.BigEndian.PutUint16(h[2:4], p.seq)
	binary.BigEndian.PutUint32(h[4:8], ts)
	binary.BigEndian.PutUint32(h[8:12], p.SSRC)
	p.seq++
	return h
}

// PacketizeNAL emits one single-NAL packet, or FU-A fragments when the NAL does
// not fit. `nal` is Annex-B; the start code is stripped.
func (p *Packetizer) PacketizeNAL(nal []byte, ts uint32, last bool) [][]byte {
	body := nal
	if len(body) >= 4 && body[0] == 0 && body[1] == 0 && body[2] == 0 && body[3] == 1 {
		body = body[4:]
	} else if len(body) >= 3 && body[0] == 0 && body[1] == 0 && body[2] == 1 {
		body = body[3:]
	}
	if len(body) == 0 {
		return nil
	}
	if len(body) <= rtpMaxPayload {
		pkt := append(p.header(ts, last), body...)
		return [][]byte{pkt}
	}

	hdr := body[0]
	nri := hdr & 0xe0
	nalType := hdr & 0x1f
	payload := body[1:]
	var out [][]byte
	for off := 0; off < len(payload); off += rtpMaxPayload - 2 {
		end := off + rtpMaxPayload - 2
		if end > len(payload) {
			end = len(payload)
		}
		start := off == 0
		fin := end == len(payload)
		fu := make([]byte, 0, 2+(end-off))
		fu = append(fu, nri|28) // FU-A
		var indicator byte = nalType
		if start {
			indicator |= 0x80
		}
		if fin {
			indicator |= 0x40
		}
		fu = append(fu, indicator)
		fu = append(fu, payload[off:end]...)
		out = append(out, append(p.header(ts, last && fin), fu...))
	}
	return out
}

// PacketizeFrame emits every packet for one frame, with the marker bit on the
// last one.
func (p *Packetizer) PacketizeFrame(fr Frame) [][]byte {
	var out [][]byte
	for i, nal := range fr.NALs {
		out = append(out, p.PacketizeNAL(nal, fr.RTPTimestamp, i == len(fr.NALs)-1)...)
	}
	return out
}

// ---------------------------------------------------------------------------
// RTCP sender report
// ---------------------------------------------------------------------------

const ntpUnixEpochDelta = uint64(2208988800)

// SenderReport builds a minimal RTCP SR asserting "RTP timestamp rtpTs was NTP
// time unixMs at the sender". This is the packet §8.5 relies on to make video
// timestamps comparable to telemetry timestamps.
func SenderReport(ssrc uint32, unixMs uint64, rtpTs uint32) []byte {
	pkt := make([]byte, 28)
	pkt[0] = 0x80
	pkt[1] = 200 // SR
	binary.BigEndian.PutUint16(pkt[2:4], 6)
	binary.BigEndian.PutUint32(pkt[4:8], ssrc)
	secs := unixMs/1000 + ntpUnixEpochDelta
	frac := ((unixMs % 1000) << 32) / 1000
	binary.BigEndian.PutUint32(pkt[8:12], uint32(secs))
	binary.BigEndian.PutUint32(pkt[12:16], uint32(frac))
	binary.BigEndian.PutUint32(pkt[16:20], rtpTs)
	return pkt
}
