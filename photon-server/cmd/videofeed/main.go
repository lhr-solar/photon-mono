// videofeed generates decodable H.264 and drives the photon-server video path
// with it.
//
// Three jobs, selected by -mode:
//
//	dump   write the generated Annex-B elementary stream to a file, so an
//	       independent decoder can confirm it is real video
//	feed   send the stream to a running server as RTP (plus one RTCP sender
//	       report), which is the path a camera uses
//	probe  connect to the server's video client port and issue video_locate and
//	       byte-range requests, printing what came back
//
// tsdb-architecture.md §8 notes there is no recorded corpus, so this is the
// substitute: the writer path is exercised by video this tool produced.
package main

import (
	"encoding/base64"
	"encoding/binary"
	"errors"
	"flag"
	"fmt"
	"io"
	"net"
	"os"
	"time"

	"server/videogen"
)

func main() {
	mode := flag.String("mode", "dump", "dump | feed | probe")
	out := flag.String("out", "gen.h264", "output path for -mode dump")
	rtpAddr := flag.String("rtp", "127.0.0.1:6200", "RTP ingress address")
	rtcpAddr := flag.String("rtcp", "127.0.0.1:6801", "RTCP address")
	videoAddr := flag.String("video", "127.0.0.1:6700", "video client address")
	gops := flag.Int("gops", 4, "number of GOPs")
	gopFrames := flag.Int("gop-frames", 45, "frames per GOP (45 @ 30fps = 1.5s IDR interval)")
	paceMs := flag.Int("pace-ms", 0, "delay per frame when feeding; 0 sends as fast as possible")
	at := flag.Uint64("at", 0, "timestamp for -mode probe")
	prefetch := flag.Int("prefetch", 4, "GOPs to prefetch for -mode probe")
	rangeOut := flag.String("range-out", "",
		"for -mode probe, write the fetched byte range here so a decoder can be pointed at it")
	in := flag.String("in", "", "for -mode html, embed this Annex-B file instead of generating one")
	sendSR := flag.Bool("sr", true, "send an RTCP sender report before feeding")
	srOffsetMs := flag.Int64("sr-offset-ms", -250,
		"sender clock minus receive clock, in ms; the skew RTCP alignment exists to remove")
	flag.Parse()

	cfg := videogen.DefaultConfig()
	cfg.Gops = *gops
	cfg.GopFrames = *gopFrames

	switch *mode {
	case "dump":
		if err := runDump(cfg, *out); err != nil {
			fail(err)
		}
	case "html":
		if err := runHTML(cfg, *out, *in); err != nil {
			fail(err)
		}
	case "feed":
		if err := runFeed(cfg, *rtpAddr, *rtcpAddr, *paceMs, *sendSR, *srOffsetMs); err != nil {
			fail(err)
		}
	case "probe":
		if err := runProbe(*videoAddr, *at, *prefetch, *rangeOut); err != nil {
			fail(err)
		}
	default:
		fail(fmt.Errorf("unknown mode %q", *mode))
	}
}

func fail(err error) {
	fmt.Fprintf(os.Stderr, "videofeed: %v\n", err)
	os.Exit(1)
}

func runDump(cfg videogen.Config, path string) error {
	frames := videogen.Stream(cfg, 1000)
	data := videogen.AnnexB(frames)
	if err := os.WriteFile(path, data, 0o644); err != nil {
		return err
	}
	keyframes := 0
	for _, f := range frames {
		if f.Keyframe {
			keyframes++
		}
	}
	fmt.Printf("wrote %s: %d bytes, %d frames, %d IDR, %dx%d, IDR interval %.2f s\n",
		path, len(data), len(frames), keyframes, cfg.WidthMBs*16, cfg.HeightMBs*16,
		float64(cfg.GopFrames)/float64(cfg.FrameRate))
	return nil
}

// runHTML writes a self-contained page that decodes the generated stream with
// the browser's WebCodecs VideoDecoder.
//
// There is no ffmpeg on this machine, and a hand-written bitstream that merely
// parses is not evidence that it decodes. A real H.264 decoder either produces
// frames from these bytes or it does not.
func runHTML(cfg videogen.Config, path string, inPath string) error {
	var stream []byte
	frameCount := 0
	keyframes := 0
	if inPath != "" {
		// Decode-check a stream that came back from the server, so the thing
		// being verified is the bytes the index handed out.
		data, err := os.ReadFile(inPath)
		if err != nil {
			return err
		}
		stream = data
		for _, t := range annexBTypes(data) {
			switch t {
			case 5:
				keyframes++
				frameCount++
			case 1:
				frameCount++
			}
		}
	} else {
		frames := videogen.Stream(cfg, 1000)
		stream = videogen.AnnexB(frames)
		frameCount = len(frames)
		for _, f := range frames {
			if f.Keyframe {
				keyframes++
			}
		}
	}
	page := fmt.Sprintf(`<!doctype html>
<meta charset="utf-8">
<title>photon videogen decode check</title>
<body style="font:13px/1.5 monospace;background:#111;color:#ddd;padding:16px">
<pre id="log">running...</pre>
<canvas id="c" width="%d" height="%d" style="image-rendering:pixelated;width:256px;border:1px solid #444"></canvas>
<script>
const B64 = "%s";
const EXPECT_FRAMES = %d, EXPECT_KEY = %d, W = %d, H = %d;
const logEl = document.getElementById('log');
const out = [];
function say(s){ out.push(s); logEl.textContent = out.join('\n'); }

function b64ToBytes(b64){
  const bin = atob(b64);
  const u8 = new Uint8Array(bin.length);
  for (let i=0;i<bin.length;i++) u8[i] = bin.charCodeAt(i);
  return u8;
}

// Split Annex-B into access units: an AU starts at an SPS (7) or at a slice
// (1/5) that follows a non-slice NAL.
function accessUnits(data){
  const starts = [];
  for (let i=0;i+4<=data.length;i++){
    if (data[i]===0 && data[i+1]===0 && data[i+2]===0 && data[i+3]===1) starts.push(i);
  }
  const nals = starts.map((s,i)=>({start:s, end: i+1<starts.length?starts[i+1]:data.length, type: data[s+4]&0x1f}));
  const aus = [];
  let cur = null;
  for (const n of nals){
    const isSlice = n.type===1 || n.type===5;
    const startsAU = n.type===7 || (isSlice && (!cur || cur.hasSlice));
    if (startsAU){ if (cur) aus.push(cur); cur = {start:n.start, end:n.end, key:false, hasSlice:false}; }
    cur.end = n.end;
    if (n.type===5){ cur.key = true; }
    if (isSlice) cur.hasSlice = true;
  }
  if (cur) aus.push(cur);
  return aus;
}

(async () => {
  if (!('VideoDecoder' in window)) { say('FAIL: no WebCodecs'); window.__result={ok:false,reason:'no webcodecs'}; return; }
  const data = b64ToBytes(B64);
  const aus = accessUnits(data);
  say('stream: ' + data.length + ' bytes, ' + aus.length + ' access units, ' +
      aus.filter(a=>a.key).length + ' keyframes');

  const support = await VideoDecoder.isConfigSupported({codec:'avc1.42001e', optimizeForLatency:true});
  say('isConfigSupported(avc1.42001e) = ' + support.supported);

  let decoded = 0, firstW = 0, firstH = 0, err = null;
  const samples = [];
  const ctx = document.getElementById('c').getContext('2d');
  const dec = new VideoDecoder({
    output: (frame) => {
      decoded++;
      if (!firstW){ firstW = frame.displayWidth; firstH = frame.displayHeight; }
      if (decoded === 1 || decoded === 46 || decoded === 91 || decoded === 136){
        ctx.drawImage(frame, 0, 0);
        const px = ctx.getImageData(0,0,1,1).data;
        samples.push({n:decoded, r:px[0], g:px[1], b:px[2]});
      }
      frame.close();
    },
    error: (e) => { err = String(e); say('decoder error: ' + err); },
  });
  dec.configure({codec:'avc1.42001e', optimizeForLatency:true});

  for (let i=0;i<aus.length;i++){
    const a = aus[i];
    if (err) break;
    dec.decode(new EncodedVideoChunk({
      type: a.key ? 'key' : 'delta',
      timestamp: Math.round(i * 1e6 / %d),
      data: data.subarray(a.start, a.end),
    }));
  }
  try { await dec.flush(); } catch(e){ err = err || String(e); }

  const ok = !err && decoded === EXPECT_FRAMES && firstW === W && firstH === H;
  say('decoded ' + decoded + ' frames at ' + firstW + 'x' + firstH +
      ' (expected ' + EXPECT_FRAMES + ' at ' + W + 'x' + H + ')');
  say('keyframe pixel samples: ' + JSON.stringify(samples));
  const distinct = new Set(samples.map(s=>s.r+','+s.g+','+s.b)).size;
  say('distinct keyframe pixels: ' + distinct + ' of ' + samples.length);
  say(ok ? 'PASS: the generated stream is real, decodable H.264'
         : 'FAIL: ' + (err || 'frame count or size mismatch'));
  window.__result = {ok, decoded, firstW, firstH, err, samples, distinct,
                     aus: aus.length, keys: aus.filter(a=>a.key).length};
})();
</script>
`, cfg.WidthMBs*16, cfg.HeightMBs*16, base64Std(stream), frameCount, keyframes,
		cfg.WidthMBs*16, cfg.HeightMBs*16, cfg.FrameRate)
	if err := os.WriteFile(path, []byte(page), 0o644); err != nil {
		return err
	}
	fmt.Printf("wrote %s (%d bytes of H.264 embedded, %d frames, %d IDR)\n",
		path, len(stream), frameCount, keyframes)
	return nil
}

func annexBTypes(data []byte) []uint8 {
	types, _ := describeAnnexB(data)
	return types
}

func base64Std(b []byte) string { return base64.StdEncoding.EncodeToString(b) }

func runFeed(cfg videogen.Config, rtpAddr, rtcpAddr string, paceMs int, sendSR bool, srOffsetMs int64) error {
	const rtpStart = uint32(1000)
	const ssrc = uint32(0x0BADCAFE)

	rtpConn, err := net.Dial("udp", rtpAddr)
	if err != nil {
		return fmt.Errorf("dial rtp: %w", err)
	}
	defer rtpConn.Close()

	if sendSR {
		rtcpConn, err := net.Dial("udp", rtcpAddr)
		if err != nil {
			return fmt.Errorf("dial rtcp: %w", err)
		}
		// The sender's clock is deliberately offset from ours. If the server
		// stamped NALs with receive time, that skew would silently become the
		// A/V error; §8.5 exists so it does not.
		senderNowMs := uint64(time.Now().UnixMilli() + srOffsetMs)
		if _, err := rtcpConn.Write(videogen.SenderReport(ssrc, senderNowMs, rtpStart)); err != nil {
			rtcpConn.Close()
			return fmt.Errorf("write rtcp: %w", err)
		}
		rtcpConn.Close()
		fmt.Printf("sent RTCP SR: rtp=%d is unix %d ms at the sender (skew %+d ms)\n",
			rtpStart, senderNowMs, srOffsetMs)
		// Let the server fold the report in before the first NAL arrives.
		time.Sleep(150 * time.Millisecond)
	}

	frames := videogen.Stream(cfg, rtpStart)
	pk := &videogen.Packetizer{SSRC: ssrc}
	packets := 0
	bytesSent := 0
	for _, fr := range frames {
		for _, pkt := range pk.PacketizeFrame(fr) {
			if _, err := rtpConn.Write(pkt); err != nil {
				return fmt.Errorf("write rtp: %w", err)
			}
			packets++
			bytesSent += len(pkt)
			// UDP to loopback will happily overrun the receiver's socket
			// buffer if a whole GOP is written in a tight loop.
			time.Sleep(200 * time.Microsecond)
		}
		if paceMs > 0 {
			time.Sleep(time.Duration(paceMs) * time.Millisecond)
		}
	}
	fmt.Printf("fed %d frames as %d RTP packets (%d bytes) to %s\n",
		len(frames), packets, bytesSent, rtpAddr)
	return nil
}

// ---------------------------------------------------------------------------
// probe: video_locate + range read over the real :6700 socket
// ---------------------------------------------------------------------------

const (
	modeLocate = uint8(3)
	modeRange  = uint8(4)
	gopRowSize = 40
)

type gopRow struct {
	startMs    uint64
	endMs      uint64
	byteOffset uint64
	byteLength uint32
	segmentID  uint32
	nalCount   uint32
	streamID   uint8
	flags      uint8
}

func runProbe(addr string, atMs uint64, prefetch int, rangeOut string) error {
	rows, err := locate(addr, atMs, prefetch)
	if err != nil {
		return err
	}
	if len(rows) == 0 {
		return errors.New("locate found no GOP")
	}
	entry := rows[0]
	payload, err := readRange(addr, entry.segmentID, entry.byteOffset, entry.byteLength)
	if err != nil {
		return err
	}
	fmt.Printf("range read: %d bytes from segment %d offset %d\n", len(payload), entry.segmentID, entry.byteOffset)
	nalTypes, keyframeFirst := describeAnnexB(payload)
	fmt.Printf("range contains NAL types %v; starts decodable: %v\n", nalTypes, keyframeFirst)
	if !keyframeFirst {
		return errors.New("the byte range a locate handed out does not start at a keyframe")
	}
	if rangeOut != "" {
		// Include every prefetched GOP too: a client walking forward from the
		// entry pointer feeds exactly this concatenation to its decoder.
		full := payload
		for _, r := range rows[1:] {
			more, err := readRange(addr, r.segmentID, r.byteOffset, r.byteLength)
			if err != nil {
				return err
			}
			full = append(full, more...)
		}
		if err := os.WriteFile(rangeOut, full, 0o644); err != nil {
			return err
		}
		fmt.Printf("wrote %s: %d bytes across %d pointer rows\n", rangeOut, len(full), len(rows))
	}
	return nil
}

func locate(addr string, atMs uint64, prefetch int) ([]gopRow, error) {
	conn, err := net.DialTimeout("tcp", addr, 3*time.Second)
	if err != nil {
		return nil, err
	}
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(5 * time.Second))

	req := make([]byte, 0, 12)
	req = append(req, modeLocate)
	var u64 [8]byte
	binary.BigEndian.PutUint64(u64[:], atMs)
	req = append(req, u64[:]...)
	req = append(req, 0) // streamId
	var u16 [2]byte
	binary.BigEndian.PutUint16(u16[:], uint16(prefetch))
	req = append(req, u16[:]...)
	if _, err := conn.Write(req); err != nil {
		return nil, err
	}

	var lenBuf [4]byte
	if _, err := io.ReadFull(conn, lenBuf[:]); err != nil {
		return nil, fmt.Errorf("read locate length: %w", err)
	}
	body := make([]byte, binary.BigEndian.Uint32(lenBuf[:]))
	if _, err := io.ReadFull(conn, body); err != nil {
		return nil, fmt.Errorf("read locate body: %w", err)
	}

	off := 0
	status := body[off]
	off++
	clockOffset := int64(binary.BigEndian.Uint64(body[off : off+8]))
	off += 8
	prov := body[off]
	off++
	residual := int64(binary.BigEndian.Uint64(body[off : off+8]))
	off += 8
	totalRows := binary.BigEndian.Uint32(body[off : off+4])
	off += 4
	minMs := binary.BigEndian.Uint64(body[off : off+8])
	off += 8
	maxMs := binary.BigEndian.Uint64(body[off : off+8])
	off += 8
	spsLen := int(binary.BigEndian.Uint16(body[off : off+2]))
	off += 2
	sps := body[off : off+spsLen]
	off += spsLen
	ppsLen := int(binary.BigEndian.Uint16(body[off : off+2]))
	off += 2
	pps := body[off : off+ppsLen]
	off += ppsLen
	rowCount := int(binary.BigEndian.Uint16(body[off : off+2]))
	off += 2

	provName := map[uint8]string{0: "receive", 1: "manual", 2: "ntp", 3: "rtcp"}[prov]
	fmt.Printf("locate at=%d status=%d rows=%d/%d span=[%d..%d] clockOffset=%+d prov=%s residual=%+d ms\n",
		atMs, status, rowCount, totalRows, minMs, maxMs, clockOffset, provName, residual)
	fmt.Printf("  sps=%d bytes pps=%d bytes\n", len(sps), len(pps))

	rows := make([]gopRow, 0, rowCount)
	for i := 0; i < rowCount; i++ {
		src := body[off : off+gopRowSize]
		off += gopRowSize
		r := gopRow{
			startMs:    binary.BigEndian.Uint64(src[0:8]),
			endMs:      binary.BigEndian.Uint64(src[8:16]),
			byteOffset: binary.BigEndian.Uint64(src[16:24]),
			byteLength: binary.BigEndian.Uint32(src[24:28]),
			segmentID:  binary.BigEndian.Uint32(src[28:32]),
			nalCount:   binary.BigEndian.Uint32(src[32:36]),
			streamID:   src[36],
			flags:      src[37],
		}
		rows = append(rows, r)
		kind := "entry"
		if i > 0 {
			kind = "prefetch"
		}
		fmt.Printf("  %-8s gop [%d..%d] seg=%d off=%d len=%d nals=%d flags=0x%02x\n",
			kind, r.startMs, r.endMs, r.segmentID, r.byteOffset, r.byteLength, r.nalCount, r.flags)
	}
	covCount := int(binary.BigEndian.Uint16(body[off : off+2]))
	off += 2
	for i := 0; i < covCount; i++ {
		seg := binary.BigEndian.Uint32(body[off : off+4])
		s := binary.BigEndian.Uint64(body[off+4 : off+12])
		e := binary.BigEndian.Uint64(body[off+12 : off+20])
		off += 20
		fmt.Printf("  coverage seg=%d [%d..%d]\n", seg, s, e)
	}
	if off != len(body) {
		return nil, fmt.Errorf("locate reply has %d trailing bytes", len(body)-off)
	}
	if status != 0 {
		return nil, nil
	}
	// The entry must be the last keyframe at or before the timestamp. Not "the
	// GOP whose [start,end] contains it": consecutive GOPs are separated by one
	// frame interval, and a cursor landing in that gap still has to resolve to
	// the keyframe before it.
	entry := rows[0]
	if atMs >= minMs && entry.startMs > atMs {
		return nil, fmt.Errorf("entry gop starts at %d, which is after at=%d", entry.startMs, atMs)
	}
	if len(rows) > 1 && rows[1].startMs <= atMs {
		return nil, fmt.Errorf("entry gop starts at %d but the next keyframe at %d is also "+
			"at or before at=%d", entry.startMs, rows[1].startMs, atMs)
	}
	_ = maxMs
	return rows, nil
}

func readRange(addr string, segmentID uint32, offset uint64, length uint32) ([]byte, error) {
	conn, err := net.DialTimeout("tcp", addr, 3*time.Second)
	if err != nil {
		return nil, err
	}
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(5 * time.Second))

	req := make([]byte, 0, 25)
	req = append(req, modeRange)
	var u64 [8]byte
	req = append(req, u64[:]...) // startTimeMs unused
	var u32 [4]byte
	binary.BigEndian.PutUint32(u32[:], segmentID)
	req = append(req, u32[:]...)
	binary.BigEndian.PutUint64(u64[:], offset)
	req = append(req, u64[:]...)
	binary.BigEndian.PutUint32(u32[:], length)
	req = append(req, u32[:]...)
	if _, err := conn.Write(req); err != nil {
		return nil, err
	}
	var lenBuf [4]byte
	if _, err := io.ReadFull(conn, lenBuf[:]); err != nil {
		return nil, fmt.Errorf("read range length: %w", err)
	}
	body := make([]byte, binary.BigEndian.Uint32(lenBuf[:]))
	if _, err := io.ReadFull(conn, body); err != nil {
		return nil, fmt.Errorf("read range body: %w", err)
	}
	return body, nil
}

// describeAnnexB lists the NAL types in a byte span and says whether a decoder
// could start at its first byte.
func describeAnnexB(data []byte) ([]uint8, bool) {
	var types []uint8
	i := 0
	for i+4 <= len(data) {
		if data[i] == 0 && data[i+1] == 0 && data[i+2] == 0 && data[i+3] == 1 {
			if i+4 < len(data) {
				types = append(types, data[i+4]&0x1f)
			}
			i += 4
			continue
		}
		i++
	}
	if len(types) == 0 {
		return nil, false
	}
	// Decodable start means SPS/PPS then IDR, or an IDR outright.
	first := types[0]
	if first == 5 {
		return types, true
	}
	if first == 7 {
		for _, t := range types {
			if t == 5 {
				return types, true
			}
		}
	}
	return types, false
}
