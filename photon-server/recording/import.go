package recording

import (
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"hash/crc32"
	"io"
	"os"
	"path/filepath"
	"server/cache"
	"server/influx"
	"server/wire"
	"sort"
)

// ImportVSD1 never modifies the originals. Restarting reuses the existing import
// and skips already copied batches, making import identities stable.
func ImportVSD1(ctx context.Context, root, path string) (*Store, error) {
	abs, err := filepath.Abs(path)
	if err != nil {
		return nil, err
	}
	stores, err := Discover(root)
	if err != nil {
		return nil, err
	}
	var target *Store
	for _, s := range stores {
		if s.Snapshot().ImportedFrom == abs {
			target = s
		} else {
			s.Close()
		}
	}
	if target == nil {
		target, err = Create(root, "import:"+abs, "legacy")
		if err != nil {
			return nil, err
		}
		if err = target.Update(func(m *Manifest) { m.ImportedFrom = abs }); err != nil {
			target.Close()
			return nil, err
		}
	}
	fail := func(err error) (*Store, error) { target.Close(); return nil, err }
	info, err := os.Stat(abs)
	if err != nil {
		return fail(err)
	}
	paths := []string{abs}
	if info.IsDir() {
		paths = nil
		err = filepath.WalkDir(abs, func(p string, d os.DirEntry, err error) error {
			if err != nil {
				return err
			}
			if !d.IsDir() && filepath.Ext(p) == ".log" {
				paths = append(paths, p)
			}
			return nil
		})
		if err != nil {
			return fail(err)
		}
	}
	sort.Strings(paths)
	if len(paths) == 0 {
		return fail(fmt.Errorf("no VSD1 logs"))
	}
	copied := target.Snapshot().Records
	var visited uint64
	var pending []byte
	for _, p := range paths {
		f, err := os.Open(p)
		if err != nil {
			return fail(err)
		}
		err = func() error {
			defer f.Close()
			for {
				select {
				case <-ctx.Done():
					return ctx.Err()
				default:
				}
				h := make([]byte, 24)
				_, err := io.ReadFull(f, h)
				if errors.Is(err, io.EOF) {
					return nil
				}
				if err != nil {
					return fmt.Errorf("truncated VSD1 header: %s", p)
				}
				size := binary.BigEndian.Uint32(h[4:])
				if binary.BigEndian.Uint32(h) != 0x56534431 || size > 1024*1024 {
					return fmt.Errorf("invalid VSD1 header: %s", p)
				}
				raw := make([]byte, size)
				if _, err = io.ReadFull(f, raw); err != nil {
					return fmt.Errorf("truncated VSD1 payload: %s", p)
				}
				crc := binary.BigEndian.Uint32(h[20:])
				if crc != 0 && crc32.ChecksumIEEE(raw) != crc {
					return fmt.Errorf("VSD1 CRC mismatch: %s", p)
				}
				pending = append(pending, raw...)
				for len(pending) > 0 {
					reader := bytes.NewReader(pending)
					batch, err := wire.ReadBatch(reader)
					if errors.Is(err, io.EOF) || errors.Is(err, io.ErrUnexpectedEOF) {
						break
					}
					if err != nil {
						return err
					}
					consumed := len(pending) - reader.Len()
					visited++
					if visited > copied {
						if _, err = target.Append(batch); err != nil {
							return err
						}
					}
					pending = pending[consumed:]
				}
				if len(pending) > wire.MaxBatchBytes {
					return fmt.Errorf("unbounded incomplete CANP batch")
				}
			}
		}()
		if err != nil {
			return fail(err)
		}
	}
	if len(pending) > 0 {
		return fail(fmt.Errorf("recording ends with an incomplete CANP batch"))
	}
	if visited < copied {
		return fail(fmt.Errorf("original recording changed since import"))
	}
	if err = target.Update(func(m *Manifest) { m.ImportedFrom = abs }); err != nil {
		return fail(err)
	}
	return target, nil
}

func InitializeInflux(ctx context.Context, c *influx.Client) error {
	var lines []string
	for _, table := range []string{"sample", "float_sample"} {
		lines = append(lines, table+",session=__schema,decode_id=__schema,signal=__schema,tap=-1 min_exact=\"0\",max_exact=\"0\",sum_exact=\"0\",count=0i,age=0i,has_age=false 0")
	}
	lines = append(lines, "legacy_sample,session=__schema,decode_id=__schema,signal=__schema,tap=-1 min_exact=\"0\",max_exact=\"0\",sum_exact=\"0\",count=0i,age=0i,has_age=false,bucket_ms=1000i,is_float=false,approximate=false 0")
	return c.Write(ctx, lines)
}

// ProjectRecording writes a complete decoding, ordered by event time. Running
// it again replaces deterministic buckets; it never adds counts to old rows.
func ProjectRecording(ctx context.Context, s *Store, dbc DBC, c *influx.Client) (Decode, error) {
	d, err := s.NewDecode(dbc.Hash, 1)
	if err != nil {
		return d, err
	}
	meta := s.Snapshot()
	acc := dbc.Defs.Clone()
	acc.SessionID = meta.ID
	acc.DecodeID = d.ID
	ids := s.RangeIDs(0, 0, meta.Records)
	var second uint64
	haveSecond := false
	finish := func(st string, checkpoint uint64, err error) {
		_ = s.Update(func(m *Manifest) {
			for i := range m.Decodes {
				if m.Decodes[i].ID == d.ID {
					m.Decodes[i].State = st
					m.Decodes[i].Checkpoint = checkpoint
					if err != nil {
						m.Decodes[i].Error = err.Error()
					} else {
						m.Decodes[i].Error = ""
					}
				}
			}
			if st == "complete" {
				m.DefaultDecode = d.ID
			}
		})
	}
	if err = InitializeInflux(ctx, c); err != nil {
		finish("failed", 0, err)
		return d, err
	}
	if err = c.Write(ctx, cache.SignalDefLines(acc, dbc.Hash, 1)); err != nil {
		finish("failed", 0, err)
		return d, err
	}
	finish("running", 0, nil)
	for _, id := range ids {
		select {
		case <-ctx.Done():
			finish("cancelled", id-1, ctx.Err())
			return d, ctx.Err()
		default:
		}
		raw, err := s.Read(id)
		if err != nil {
			finish("failed", id-1, err)
			return d, err
		}
		b, err := wire.ReadBatch(bytes.NewReader(raw))
		if err != nil {
			finish("failed", id-1, err)
			return d, err
		}
		sec := b.TimeMS / 1000
		if haveSecond && sec != second {
			if err = c.Write(ctx, acc.OpenLines(meta.ID)); err != nil {
				finish("failed", id-1, err)
				return d, err
			}
			acc.DropClosed(int64(sec))
			finish("running", id-1, nil)
		}
		second = sec
		haveSecond = true
		for _, f := range b.Frames {
			f.Bus = meta.Bus
			acc.Fold(f)
		}
	}
	if err = c.Write(ctx, acc.OpenLines(meta.ID)); err != nil {
		finish("failed", meta.Records, err)
		return d, err
	}
	finish("complete", meta.Records, nil)
	d.State = "complete"
	d.Checkpoint = meta.Records
	return d, nil
}
