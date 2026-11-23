package main

// A compressed chunk (.segz) holds the same bytes as its .seg file, packed.
// Decompressing it gives back the .seg bytes exactly: record headers, write
// times, crcs, batch headers and any bytes after the last whole record.
//
// File layout:
//
//	magic "PSZ1" | version 1 | 3 reserved bytes | u64 original size | sha256 of the original
//	then blocks until EOF: u32 original size | u32 zstd size | zstd frame
//
// A block's decoded content starts with a kind byte. Whole records are split
// into streams first (see encodeRecordBlock), so zstd sees like things together.
// The bytes after the last whole record are kept as they are.

import (
	"bufio"
	"context"
	"crypto/sha256"
	"encoding/binary"
	"errors"
	"fmt"
	"hash"
	"hash/crc32"
	"io"
	"math"
	"os"
	"slices"

	"github.com/klauspost/compress/zstd"
)

const (
	chunkzMagic       = "PSZ1"
	chunkzVersion     = 1
	chunkzHeaderBytes = 48 // magic, version, reserved, size, sha256
	chunkzExt         = ".segz"
	chunkzTmpExt      = ".segz.tmp"
	// chunkzBlockBytes is about how much of the original one block holds. A
	// record bigger than this is a block of its own.
	chunkzBlockBytes = 4 << 20
	// chunkzWindowBytes is the zstd window.
	chunkzWindowBytes = 4 << 20

	// The kinds a block's decoded content can start with.
	chunkzKindRecords = 'R' // whole records, split into streams
	chunkzKindRaw     = 'B' // bytes as they are: the tail after the last whole record

	// Flags at the start of a record's entry in the record stream.
	chunkzFlagBatches = 1 // the payload is whole CANP v3 batches
	chunkzFlagCRC     = 2 // the stored crc is not crc32 of the payload; it follows
)

// errChunkzVerify means a compressed copy does not give back the original bytes.
var errChunkzVerify = errors.New("compressed chunk does not match the original")

// errChunkzBlock means a block's content does not parse.
var errChunkzBlock = errors.New("compressed chunk: bad block")

// chunkzAfterWrite is a test hook. It runs on the temporary file after it is
// written and before it is checked. It is nil in the server.
var chunkzAfterWrite func(tmp string)

// chunkzInfo says what a compressed chunk holds.
type chunkzInfo struct {
	Size   int64    // bytes of the original .seg file
	Sha256 [32]byte // sha256 of the original bytes
	ZBytes int64    // bytes of the .segz file
}

// chunkzFileName is the compressed file of chunk number n.
func chunkzFileName(n int) string {
	return fmt.Sprintf("%06d.segz", n)
}

// chunkzMem is the memory a worker reuses from block to block. Writing and checking
// run one after the other, so they share the byte buffers.
type chunkzMem struct {
	blocks  blockScratch  // splits records into streams
	dec     decodeScratch // rebuilds records from streams
	zbuf    []byte        // a block's zstd bytes, written or read
	content []byte        // a block's decoded content when reading; a raw block's content when writing
	orig    []byte        // a block's original bytes when reading
}

// chunkzWorker holds a zstd encoder and decoder and the memory they reuse. One
// goroutine uses a worker at a time.
type chunkzWorker struct {
	enc *zstd.Encoder
	dec *zstd.Decoder
	mem chunkzMem
}

func newChunkzWorker() (*chunkzWorker, error) {
	enc, err := zstd.NewWriter(nil,
		zstd.WithEncoderLevel(zstd.SpeedBetterCompression),
		zstd.WithWindowSize(chunkzWindowBytes),
		zstd.WithEncoderConcurrency(1),
		zstd.WithLowerEncoderMem(true),
	)
	if err != nil {
		return nil, err
	}
	dec, err := zstd.NewReader(nil, zstd.WithDecoderConcurrency(1))
	if err != nil {
		_ = enc.Close()
		return nil, err
	}
	return &chunkzWorker{enc: enc, dec: dec}, nil
}

// close frees the encoder and decoder.
func (w *chunkzWorker) close() {
	_ = w.enc.Close()
	w.dec.Close()
}

// compressChunk writes the chunk at src to dst as a .segz file. It writes
// dst+".tmp", reads that back and checks it against src, then renames it to dst.
func compressChunk(src, dst string) (chunkzInfo, error) {
	w, err := newChunkzWorker()
	if err != nil {
		return chunkzInfo{}, err
	}
	defer w.close()
	return w.compress(context.Background(), src, dst)
}

// compress writes src to dst through dst+".tmp". A stopped context or a failed
// check removes the temporary file and leaves dst alone. A failed check returns
// an error that wraps errChunkzVerify.
func (w *chunkzWorker) compress(ctx context.Context, src, dst string) (info chunkzInfo, err error) {
	in, err := os.Open(src)
	if err != nil {
		return info, err
	}
	defer in.Close()
	st, err := in.Stat()
	if err != nil {
		return info, err
	}
	size := st.Size()

	tmp := dst + ".tmp"
	out, err := os.OpenFile(tmp, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o644)
	if err != nil {
		return info, err
	}
	closed := false
	defer func() {
		if !closed {
			_ = out.Close()
		}
		if err != nil {
			_ = os.Remove(tmp)
		}
	}()

	// The sha256 is only known at the end, so the header is written again then.
	hdr := make([]byte, chunkzHeaderBytes)
	copy(hdr, chunkzMagic)
	hdr[4] = chunkzVersion
	binary.BigEndian.PutUint64(hdr[8:16], uint64(size))
	if _, err = out.Write(hdr); err != nil {
		return info, err
	}
	written := int64(len(hdr))
	sum := sha256.New()

	r := bufio.NewReaderSize(in, scanBufferBytes)
	var recs []byte // the whole records of the block being filled
	flush := func() error {
		if len(recs) == 0 {
			return nil
		}
		n, werr := w.writeBlock(out, chunkzKindRecords, recs, sum)
		written += n
		recs = recs[:0]
		return werr
	}

	// Whole records first: a header that is complete, has the record magic and
	// fits in the file. The rest of the file is the tail.
	var off int64
	for off < size {
		if err = ctx.Err(); err != nil {
			return info, err
		}
		peek, perr := r.Peek(frameHeaderBytes)
		if perr != nil || binary.BigEndian.Uint32(peek[0:4]) != frameMagic {
			break
		}
		total := frameHeaderBytes + int64(binary.BigEndian.Uint32(peek[4:8]))
		if off+total > size {
			break
		}
		if total > chunkzBlockBytes {
			// A record bigger than a block is a block of its own.
			if err = flush(); err != nil {
				return info, err
			}
			rec := make([]byte, total)
			if _, err = io.ReadFull(r, rec); err != nil {
				return info, err
			}
			var n int64
			n, err = w.writeBlock(out, chunkzKindRecords, rec, sum)
			if err != nil {
				return info, err
			}
			written += n
		} else {
			if int64(len(recs))+total > chunkzBlockBytes {
				if err = flush(); err != nil {
					return info, err
				}
			}
			start := len(recs)
			recs = slices.Grow(recs, int(total))[:start+int(total)]
			if _, err = io.ReadFull(r, recs[start:]); err != nil {
				return info, err
			}
		}
		off += total
	}
	if err = flush(); err != nil {
		return info, err
	}
	var tail []byte // one buffer for the raw tail blocks
	for off < size {
		if err = ctx.Err(); err != nil {
			return info, err
		}
		n := min(size-off, chunkzBlockBytes)
		tail = slices.Grow(tail[:0], int(n))[:n]
		if _, err = io.ReadFull(r, tail); err != nil {
			return info, err
		}
		var wn int64
		wn, err = w.writeBlock(out, chunkzKindRaw, tail, sum)
		if err != nil {
			return info, err
		}
		written += wn
		off += n
	}

	var digest [32]byte
	sum.Sum(digest[:0])
	if _, err = out.WriteAt(digest[:], 16); err != nil {
		return info, err
	}
	if err = out.Sync(); err != nil {
		return info, err
	}
	closed = true
	if err = out.Close(); err != nil {
		return info, err
	}
	info = chunkzInfo{Size: size, Sha256: digest, ZBytes: written}

	if chunkzAfterWrite != nil {
		chunkzAfterWrite(tmp)
	}
	if err = w.verify(tmp, info); err != nil {
		return info, err
	}
	if err = renameWithRetry(tmp, dst); err != nil {
		return info, err
	}
	return info, nil
}

// writeBlock compresses raw as one block and writes it to out. It hashes raw
// into sum. It returns the bytes written.
func (w *chunkzWorker) writeBlock(out io.Writer, kind byte, raw []byte, sum hash.Hash) (int64, error) {
	if uint64(len(raw)) > math.MaxUint32 {
		return 0, errors.New("compressed chunk: block too big")
	}
	sum.Write(raw)
	var content []byte
	if kind == chunkzKindRecords {
		content = encodeRecordBlock(raw, &w.mem.blocks)
	} else {
		w.mem.content = append(append(w.mem.content[:0], chunkzKindRaw), raw...)
		content = w.mem.content
	}
	w.mem.zbuf = w.enc.EncodeAll(content, w.mem.zbuf[:0])
	var h [8]byte
	binary.BigEndian.PutUint32(h[0:4], uint32(len(raw)))
	binary.BigEndian.PutUint32(h[4:8], uint32(len(w.mem.zbuf)))
	if _, err := out.Write(h[:]); err != nil {
		return 0, err
	}
	if _, err := out.Write(w.mem.zbuf); err != nil {
		return 8, err
	}
	return int64(8 + len(w.mem.zbuf)), nil
}

// verify reads the file at path back and checks that it gives the original bytes
// described by want. Any mismatch returns an error wrapping errChunkzVerify.
func (w *chunkzWorker) verify(path string, want chunkzInfo) error {
	z, err := newChunkzReader(path, w.dec, &w.mem)
	if err != nil {
		return fmt.Errorf("%w: %v", errChunkzVerify, err)
	}
	defer z.Close()
	h := sha256.New()
	n, err := io.Copy(h, z)
	if err != nil {
		return fmt.Errorf("%w: %v", errChunkzVerify, err)
	}
	var digest [32]byte
	h.Sum(digest[:0])
	if n != want.Size || digest != want.Sha256 {
		return fmt.Errorf("%w: %d bytes with sha256 %x, want %d with %x",
			errChunkzVerify, n, digest, want.Size, want.Sha256)
	}
	return nil
}

// verifyExisting checks a .segz file that is already on disk against the .seg
// file it was made from. It returns what the .seg holds.
func (w *chunkzWorker) verifyExisting(src, zpath string) (chunkzInfo, error) {
	in, err := os.Open(src)
	if err != nil {
		return chunkzInfo{}, err
	}
	defer in.Close()
	h := sha256.New()
	n, err := io.Copy(h, in)
	if err != nil {
		return chunkzInfo{}, err
	}
	st, err := os.Stat(zpath)
	if err != nil {
		return chunkzInfo{}, err
	}
	info := chunkzInfo{Size: n, ZBytes: st.Size()}
	h.Sum(info.Sha256[:0])
	if err := w.verify(zpath, info); err != nil {
		return chunkzInfo{}, err
	}
	return info, nil
}

// chunkzReader yields the original bytes of a .segz file, one block at a time.
type chunkzReader struct {
	f      *os.File
	dec    *zstd.Decoder
	ownDec bool       // dec was made here and is closed with the reader
	mem    *chunkzMem // the buffers this reader uses
	size   int64      // original size, from the header
	sum    [32]byte   // sha256 of the original, from the header
	got    int64      // original bytes read so far
	hash   hash.Hash
	blk    []byte // the current block's original bytes
	pos    int    // how much of blk has been returned
	err    error  // sticky: io.EOF at the end, or the first failure
}

// newChunkzReader opens a .segz file and reads its header. A nil dec makes a
// decoder that Close frees; a nil mem makes buffers the reader keeps to itself.
func newChunkzReader(path string, dec *zstd.Decoder, mem *chunkzMem) (*chunkzReader, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	fail := func(err error) (*chunkzReader, error) {
		_ = f.Close()
		return nil, err
	}
	hdr := make([]byte, chunkzHeaderBytes)
	if _, err := io.ReadFull(f, hdr); err != nil {
		return fail(fmt.Errorf("%s: header: %w", path, err))
	}
	if string(hdr[0:4]) != chunkzMagic || hdr[4] != chunkzVersion {
		return fail(fmt.Errorf("%s: not a version %d compressed chunk", path, chunkzVersion))
	}
	size := binary.BigEndian.Uint64(hdr[8:16])
	if size > math.MaxInt64 {
		return fail(fmt.Errorf("%s: bad size in header", path))
	}
	z := &chunkzReader{f: f, dec: dec, mem: mem, size: int64(size), hash: sha256.New()}
	copy(z.sum[:], hdr[16:48])
	if z.mem == nil {
		z.mem = &chunkzMem{}
	}
	if z.dec == nil {
		d, err := zstd.NewReader(nil, zstd.WithDecoderConcurrency(1))
		if err != nil {
			return fail(err)
		}
		z.dec, z.ownDec = d, true
	}
	return z, nil
}

// openChunkz opens a .segz file. Reading it yields the bytes of the .seg file.
func openChunkz(path string) (io.ReadCloser, error) {
	z, err := newChunkzReader(path, nil, nil)
	if err != nil {
		return nil, err
	}
	return z, nil
}

// Close closes the file, and the decoder if this reader made it.
func (z *chunkzReader) Close() error {
	if z.ownDec {
		z.dec.Close()
	}
	return z.f.Close()
}

// Read returns original bytes. At the end it checks the size and sha256 against
// the header, and returns io.EOF.
func (z *chunkzReader) Read(p []byte) (int, error) {
	for z.pos == len(z.blk) {
		if z.err != nil {
			return 0, z.err
		}
		z.err = z.nextBlock()
	}
	n := copy(p, z.blk[z.pos:])
	z.pos += n
	return n, nil
}

// nextBlock reads and decodes the next block. At the end of the file it checks
// the total against the header and returns io.EOF.
func (z *chunkzReader) nextBlock() error {
	var h [8]byte
	if _, err := io.ReadFull(z.f, h[:]); err != nil {
		if !errors.Is(err, io.EOF) {
			return fmt.Errorf("compressed chunk: %w", err)
		}
		if z.got != z.size {
			return fmt.Errorf("compressed chunk holds %d bytes, its header says %d", z.got, z.size)
		}
		var digest [32]byte
		z.hash.Sum(digest[:0])
		if digest != z.sum {
			return errors.New("compressed chunk does not match the sha256 in its header")
		}
		return io.EOF
	}
	rawLen := int64(binary.BigEndian.Uint32(h[0:4]))
	compLen := int(binary.BigEndian.Uint32(h[4:8]))
	if rawLen > z.size-z.got || compLen == 0 {
		return errChunkzBlock
	}
	m := z.mem
	m.zbuf = slices.Grow(m.zbuf[:0], compLen)[:compLen]
	if _, err := io.ReadFull(z.f, m.zbuf); err != nil {
		return fmt.Errorf("compressed chunk: %w", err)
	}
	content, err := z.dec.DecodeAll(m.zbuf, m.content[:0])
	if err != nil {
		return fmt.Errorf("compressed chunk: %w", err)
	}
	m.content = content
	blk, err := decodeChunkzBlock(content, m.orig[:0], &m.dec)
	if err != nil {
		return err
	}
	m.orig = blk
	if int64(len(blk)) != rawLen {
		return fmt.Errorf("compressed chunk: block holds %d bytes, its header says %d", len(blk), rawLen)
	}
	z.hash.Write(blk)
	z.got += rawLen
	z.blk, z.pos = blk, 0
	return nil
}

// decodeChunkzBlock turns a block's decoded content back into original bytes,
// appended to dst.
func decodeChunkzBlock(content, dst []byte, ds *decodeScratch) ([]byte, error) {
	if len(content) == 0 {
		return nil, errChunkzBlock
	}
	switch content[0] {
	case chunkzKindRaw:
		return append(dst, content[1:]...), nil
	case chunkzKindRecords:
		return decodeRecordBlock(content[1:], dst, ds)
	}
	return nil, fmt.Errorf("compressed chunk: unknown block kind %q", content[0])
}

// batchesIn reports how many CANP v3 batches p holds, when p is nothing but whole
// batches that end exactly at its end.
func batchesIn(p []byte) (int, bool) {
	n := 0
	for o := 0; o < len(p); n++ {
		if len(p)-o < canpHeaderSize || binary.BigEndian.Uint32(p[o:]) != canpMagic ||
			binary.BigEndian.Uint16(p[o+4:]) != canpVersion {
			return 0, false
		}
		count := int(binary.BigEndian.Uint16(p[o+6:]))
		if count == 0 || o+canpHeaderSize+canpFrameSize*count > len(p) {
			return 0, false
		}
		o += canpHeaderSize + canpFrameSize*count
	}
	return n, n > 0
}

// forEachFrame calls fn with each CANP frame of the whole-batch records in raw, in
// order. It finds them the way encodeRecordBlock does.
func forEachFrame(raw []byte, fn func(f []byte)) {
	for off := 0; off < len(raw); {
		n := int(binary.BigEndian.Uint32(raw[off+4 : off+8]))
		payload := raw[off+frameHeaderBytes : off+frameHeaderBytes+n]
		off += frameHeaderBytes + n
		if _, whole := batchesIn(payload); !whole {
			continue
		}
		for o := 0; o < len(payload); {
			count := int(binary.BigEndian.Uint16(payload[o+6:]))
			o += canpHeaderSize
			for k := 0; k < count; k++ {
				fn(payload[o : o+canpFrameSize])
				o += canpFrameSize
			}
		}
	}
}

// blockScratch is the memory encodeRecordBlock reuses from block to block, so a
// compression does not allocate it again for every block.
type blockScratch struct {
	rec, bat, fidx, opq, ids, out []byte
	dlc, dat, dt                  []byte
	idIndex                       map[uint32]uint32
	idTab                         []uint32
	idxs                          []uint32 // the ID index of each frame, in order
	counts, starts, seen          []int
	prevData                      [][8]byte
	prevDT                        [][16]byte
}

// resized returns s with n elements, all zero, reusing its memory when it can.
func resized[T any](s []T, n int) []T {
	s = slices.Grow(s[:0], n)[:n]
	clear(s)
	return s
}

// encodeRecordBlock splits whole records (header and payload, as the .seg file
// has them) into the streams of a records block, and returns the block's content.
// The content is valid until the next call with the same scratch.
//
// The content is the kind byte, the record count, the length of each of the eight
// streams (uvarints), then the streams in that order:
//
//	rec    per record: flags, id delta, payload length, write time delta, and the
//	       stored crc when it is not crc32 of the payload
//	bat    per batch payload: batch count; per batch: count, seq delta, ts delta
//	ids    uvarint count, then each frame ID (uint32, big endian)
//	fidx   per frame, in order: its index in ids
//	dlc    the dlc byte of each frame, grouped by ID
//	dat    the 8 data bytes of each frame, XOR the previous frame of its ID, grouped by ID
//	dt     the 16 dt bytes of each frame, XOR the previous frame of its ID, grouped by ID
//	opq    the payloads that are not whole batches, as they are
//
// Deltas are against the previous record or batch in the block.
func encodeRecordBlock(raw []byte, s *blockScratch) []byte {
	rec, bat, fidx, opq := s.rec[:0], s.bat[:0], s.fidx[:0], s.opq[:0]
	if s.idIndex == nil {
		s.idIndex = map[uint32]uint32{}
	}
	idIndex := s.idIndex
	clear(idIndex)
	idTab := s.idTab[:0]
	idxs := s.idxs[:0]
	var (
		nrec    int
		prevID  MessageID
		prevW   int64 // write time of the previous record
		prevSeq int64
		prevTS  uint64
	)
	for off := 0; off < len(raw); {
		hdr := raw[off : off+frameHeaderBytes]
		n := int(binary.BigEndian.Uint32(hdr[4:8]))
		id := MessageID(binary.BigEndian.Uint64(hdr[8:16]))
		wu := binary.BigEndian.Uint32(hdr[16:20])
		stored := binary.BigEndian.Uint32(hdr[20:24])
		payload := raw[off+frameHeaderBytes : off+frameHeaderBytes+n]
		off += frameHeaderBytes + n
		nrec++

		nb, whole := batchesIn(payload)
		var flags byte
		if whole {
			flags |= chunkzFlagBatches
		}
		if stored != crc32.ChecksumIEEE(payload) {
			flags |= chunkzFlagCRC
		}
		rec = append(rec, flags)
		rec = binary.AppendVarint(rec, int64(id-prevID))
		rec = binary.AppendUvarint(rec, uint64(n))
		rec = binary.AppendVarint(rec, int64(wu)-prevW)
		if flags&chunkzFlagCRC != 0 {
			rec = binary.BigEndian.AppendUint32(rec, stored)
		}
		prevID, prevW = id, int64(wu)

		if !whole {
			opq = append(opq, payload...)
			continue
		}
		bat = binary.AppendUvarint(bat, uint64(nb))
		for o := 0; o < len(payload); {
			count := int(binary.BigEndian.Uint16(payload[o+6:]))
			seq := binary.BigEndian.Uint32(payload[o+8:])
			ts := binary.BigEndian.Uint64(payload[o+12:])
			bat = binary.AppendUvarint(bat, uint64(count))
			bat = binary.AppendVarint(bat, int64(seq)-prevSeq)
			bat = binary.AppendVarint(bat, int64(ts-prevTS))
			prevSeq, prevTS = int64(seq), ts
			o += canpHeaderSize
			for k := 0; k < count; k++ {
				fid := binary.BigEndian.Uint32(payload[o : o+4])
				o += canpFrameSize
				i, ok := idIndex[fid]
				if !ok {
					i = uint32(len(idTab))
					idIndex[fid] = i
					idTab = append(idTab, fid)
				}
				fidx = binary.AppendUvarint(fidx, uint64(i))
				idxs = append(idxs, i)
			}
		}
	}
	s.rec, s.bat, s.fidx, s.opq, s.idTab, s.idxs = rec, bat, fidx, opq, idTab, idxs

	// Group the frames by ID. Each frame's data and dt are XORed with the previous
	// frame of the same ID, so the group holds small numbers. The frames are read
	// again from raw, so no copy of them is kept.
	n := len(idTab)
	s.counts = resized(s.counts, n)
	for _, i := range idxs {
		s.counts[i]++
	}
	s.starts = slices.Grow(s.starts[:0], n)[:n]
	total := 0
	for i, c := range s.counts {
		s.starts[i] = total
		total += c
	}
	s.dlc = slices.Grow(s.dlc[:0], total)[:total]
	s.dat = slices.Grow(s.dat[:0], 8*total)[:8*total]
	s.dt = slices.Grow(s.dt[:0], 16*total)[:16*total]
	s.seen = resized(s.seen, n)
	s.prevData = resized(s.prevData, n)
	s.prevDT = resized(s.prevDT, n)
	k := 0
	forEachFrame(raw, func(f []byte) {
		i := idxs[k]
		k++
		p := s.starts[i] + s.seen[i]
		s.seen[i]++
		s.dlc[p] = f[4]
		for q := 0; q < 8; q++ {
			s.dat[8*p+q] = f[5+q] ^ s.prevData[i][q]
		}
		for q := 0; q < 16; q++ {
			s.dt[16*p+q] = f[13+q] ^ s.prevDT[i][q]
		}
		copy(s.prevData[i][:], f[5:13])
		copy(s.prevDT[i][:], f[13:29])
	})

	s.ids = binary.AppendUvarint(s.ids[:0], uint64(n))
	for _, id := range idTab {
		s.ids = binary.BigEndian.AppendUint32(s.ids, id)
	}

	streams := [][]byte{rec, bat, s.ids, fidx, s.dlc, s.dat, s.dt, opq}
	out := append(s.out[:0], chunkzKindRecords)
	out = binary.AppendUvarint(out, uint64(nrec))
	for _, st := range streams {
		out = binary.AppendUvarint(out, uint64(len(st)))
	}
	for _, st := range streams {
		out = append(out, st...)
	}
	s.out = out
	return out
}

// streamReader reads the values of one stream. The first bad read sets err, and
// later reads return zero.
type streamReader struct {
	b   []byte
	err error
}

func (r *streamReader) fail() {
	if r.err == nil {
		r.err = errChunkzBlock
	}
}

// take returns the next n bytes, or nil and an error when there are not that many.
func (r *streamReader) take(n int) []byte {
	if r.err != nil || n < 0 || n > len(r.b) {
		r.fail()
		return nil
	}
	v := r.b[:n]
	r.b = r.b[n:]
	return v
}

func (r *streamReader) u8() byte {
	if b := r.take(1); b != nil {
		return b[0]
	}
	return 0
}

func (r *streamReader) uvarint() uint64 {
	if r.err != nil {
		return 0
	}
	v, n := binary.Uvarint(r.b)
	if n <= 0 {
		r.fail()
		return 0
	}
	r.b = r.b[n:]
	return v
}

func (r *streamReader) varint() int64 {
	if r.err != nil {
		return 0
	}
	v, n := binary.Varint(r.b)
	if n <= 0 {
		r.fail()
		return 0
	}
	r.b = r.b[n:]
	return v
}

func (r *streamReader) u32() uint32 {
	b := r.take(4)
	if b == nil {
		return 0
	}
	return binary.BigEndian.Uint32(b)
}

// blockRecord is one record of a records block, as the decoder reads it.
type blockRecord struct {
	id    MessageID
	wu    uint32
	n     int
	flags byte
	crc   uint32
	nb    int // batches in the payload, when it is batches
}

// blockBatch is one batch of a records block.
type blockBatch struct {
	count int
	seq   uint32
	ts    uint64
}

// decodeScratch is the memory decodeRecordBlock reuses from block to block.
type decodeScratch struct {
	metas                []blockRecord
	batches              []blockBatch
	idTab                []uint32
	fi                   []uint32 // the ID index of each frame, in order
	counts, starts, seen []int
	prevData             [][8]byte
	prevDT               [][16]byte
}

// decodeRecordBlock rebuilds the records of a records block from its content,
// appended to dst. It is the inverse of encodeRecordBlock.
func decodeRecordBlock(content, dst []byte, ds *decodeScratch) ([]byte, error) {
	head := streamReader{b: content}
	nrec := head.uvarint()
	var lens [8]int
	for i := range lens {
		lens[i] = int(head.uvarint())
	}
	if head.err != nil || nrec > uint64(len(content)) {
		return nil, errChunkzBlock
	}
	var streams [8][]byte
	for i := range streams {
		streams[i] = head.take(lens[i])
	}
	if head.err != nil {
		return nil, errChunkzBlock
	}
	rec := streamReader{b: streams[0]}
	bat := streamReader{b: streams[1]}
	ids := streamReader{b: streams[2]}
	fidx := streamReader{b: streams[3]}
	dlc, dat, dt, opq := streams[4], streams[5], streams[6], streams[7]

	metas := ds.metas[:0]
	var (
		prevID MessageID
		prevW  int64
	)
	for i := uint64(0); i < nrec; i++ {
		var m blockRecord
		m.flags = rec.u8()
		m.id = prevID + MessageID(rec.varint())
		m.n = int(rec.uvarint())
		m.wu = uint32(prevW + rec.varint())
		if m.flags&chunkzFlagCRC != 0 {
			m.crc = rec.u32()
		}
		prevID, prevW = m.id, int64(m.wu)
		metas = append(metas, m)
	}
	ds.metas = metas
	if rec.err != nil {
		return nil, rec.err
	}

	batches := ds.batches[:0]
	var (
		prevSeq     int64
		prevTS      uint64
		totalFrames int
	)
	for i := range metas {
		if metas[i].flags&chunkzFlagBatches == 0 {
			continue
		}
		nb := int(bat.uvarint())
		if bat.err != nil || nb < 0 || nb > len(bat.b) {
			return nil, errChunkzBlock
		}
		metas[i].nb = nb
		for j := 0; j < nb; j++ {
			count := int(bat.uvarint())
			seq := uint32(prevSeq + bat.varint())
			prevSeq = int64(seq)
			ts := prevTS + uint64(bat.varint())
			prevTS = ts
			if count < 1 || count > math.MaxUint16 {
				return nil, errChunkzBlock
			}
			batches = append(batches, blockBatch{count: count, seq: seq, ts: ts})
			totalFrames += count
		}
	}
	ds.batches = batches
	if bat.err != nil {
		return nil, bat.err
	}

	nIDs := int(ids.uvarint())
	if ids.err != nil || nIDs < 0 || nIDs > len(ids.b)/4 {
		return nil, errChunkzBlock
	}
	idTab := ds.idTab[:0]
	for i := 0; i < nIDs; i++ {
		idTab = append(idTab, ids.u32())
	}
	ds.idTab = idTab
	if ids.err != nil {
		return nil, ids.err
	}

	if totalFrames > len(fidx.b) || len(dlc) != totalFrames || len(dat) != 8*totalFrames || len(dt) != 16*totalFrames {
		return nil, errChunkzBlock
	}
	fi := slices.Grow(ds.fi[:0], totalFrames)[:totalFrames]
	ds.fi = fi
	counts := resized(ds.counts, nIDs)
	ds.counts = counts
	for k := range fi {
		v := fidx.uvarint()
		if fidx.err != nil || v >= uint64(nIDs) {
			return nil, errChunkzBlock
		}
		fi[k] = uint32(v)
		counts[v]++
	}
	starts := slices.Grow(ds.starts[:0], nIDs)[:nIDs]
	ds.starts = starts
	total := 0
	for i, c := range counts {
		starts[i] = total
		total += c
	}
	ds.seen = resized(ds.seen, nIDs)
	ds.prevData = resized(ds.prevData, nIDs)
	ds.prevDT = resized(ds.prevDT, nIDs)
	seen, prevData, prevDT := ds.seen, ds.prevData, ds.prevDT

	out := dst
	var (
		fpos, bpos, oo int
		zeros          [frameHeaderBytes]byte
	)
	for _, m := range metas {
		start := len(out)
		out = append(out, zeros[:]...)
		payloadStart := len(out)
		if m.flags&chunkzFlagBatches != 0 {
			for j := 0; j < m.nb; j++ {
				b := batches[bpos]
				bpos++
				out = binary.BigEndian.AppendUint32(out, canpMagic)
				out = binary.BigEndian.AppendUint16(out, canpVersion)
				out = binary.BigEndian.AppendUint16(out, uint16(b.count))
				out = binary.BigEndian.AppendUint32(out, b.seq)
				out = binary.BigEndian.AppendUint64(out, b.ts)
				for k := 0; k < b.count; k++ {
					i := fi[fpos]
					fpos++
					p := starts[i] + seen[i]
					seen[i]++
					var f [canpFrameSize]byte
					binary.BigEndian.PutUint32(f[0:4], idTab[i])
					f[4] = dlc[p]
					for q := 0; q < 8; q++ {
						f[5+q] = dat[8*p+q] ^ prevData[i][q]
					}
					for q := 0; q < 16; q++ {
						f[13+q] = dt[16*p+q] ^ prevDT[i][q]
					}
					copy(prevData[i][:], f[5:13])
					copy(prevDT[i][:], f[13:29])
					out = append(out, f[:]...)
				}
			}
		} else {
			if m.n < 0 || m.n > len(opq)-oo {
				return nil, errChunkzBlock
			}
			out = append(out, opq[oo:oo+m.n]...)
			oo += m.n
		}
		payload := out[payloadStart:]
		if len(payload) != m.n {
			return nil, errChunkzBlock
		}
		h := out[start:payloadStart]
		binary.BigEndian.PutUint32(h[0:4], frameMagic)
		binary.BigEndian.PutUint32(h[4:8], uint32(m.n))
		binary.BigEndian.PutUint64(h[8:16], uint64(m.id))
		binary.BigEndian.PutUint32(h[16:20], m.wu)
		crc := m.crc
		if m.flags&chunkzFlagCRC == 0 {
			crc = crc32.ChecksumIEEE(payload)
		}
		binary.BigEndian.PutUint32(h[20:24], crc)
	}
	if bpos != len(batches) || fpos != totalFrames || oo != len(opq) {
		return nil, errChunkzBlock
	}
	return out, nil
}
