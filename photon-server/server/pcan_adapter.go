package main

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"math"
	"net"
	"net/http"
	"server/cache"
	"server/wire"
	"strconv"
	"time"
)

type pcanState struct {
	owner      *browserClient
	armed      bool
	listenOnly bool
	commands   []any
	seen       time.Time
	available  bool
	channel    int
	heartbeat  time.Time
}

func localRequest(r *http.Request) bool {
	host, _, _ := net.SplitHostPort(r.RemoteAddr)
	ip := net.ParseIP(host)
	return ip != nil && ip.IsLoopback()
}
func (b *browserService) ingestPCAN(w http.ResponseWriter, r *http.Request) {
	if !localRequest(r) {
		http.Error(w, "adapter must connect over loopback", 403)
		return
	}
	r.Body = http.MaxBytesReader(w, r.Body, 1024*1024)
	data, err := io.ReadAll(r.Body)
	if err != nil {
		http.Error(w, err.Error(), 413)
		return
	}
	reader := bytes.NewReader(data)
	channel, _ := strconv.Atoi(r.URL.Query().Get("channel"))
	if channel < 1 || channel > 8 {
		http.Error(w, "channel must be 1..8", 400)
		return
	}
	source := fmt.Sprintf("pcan:usb:%d", channel)
	for reader.Len() > 0 {
		batch, err := wire.ReadBatch(reader)
		if err != nil {
			http.Error(w, err.Error(), 400)
			return
		}
		if err = b.recordings.Ingest(source, batch); err != nil {
			http.Error(w, err.Error(), 503)
			return
		}
	}
	b.pcanMu.Lock()
	b.pcan.seen = time.Now()
	b.pcanMu.Unlock()
	w.WriteHeader(204)
}
func (b *browserService) pcanControl(w http.ResponseWriter, r *http.Request) {
	if !localRequest(r) {
		http.Error(w, "loopback only", 403)
		return
	}
	b.pcanMu.Lock()
	b.pcan.seen = time.Now()
	commands := b.pcan.commands
	b.pcan.commands = nil
	if time.Since(b.pcan.heartbeat) > 3*time.Second || !b.pcan.available || !b.pcan.armed {
		filtered := commands[:0]
		for _, command := range commands {
			if item, ok := command.(map[string]any); ok && item["type"] != "pcan_tx" {
				filtered = append(filtered, command)
			}
		}
		commands = filtered
		b.pcan.armed = false
	}
	b.pcanMu.Unlock()
	jsonReply(w, 200, map[string]any{"commands": commands})
}
func (b *browserService) pcanStatus(w http.ResponseWriter, r *http.Request) {
	if !localRequest(r) {
		http.Error(w, "loopback only", 403)
		return
	}
	r.Body = http.MaxBytesReader(w, r.Body, 4096)
	var status map[string]any
	if err := json.NewDecoder(r.Body).Decode(&status); err != nil {
		http.Error(w, err.Error(), 400)
		return
	}
	b.pcanMu.Lock()
	b.pcan.seen = time.Now()
	if available, ok := status["available"].(bool); ok {
		b.pcan.available = available
	}
	if message, ok := status["error"].(string); ok && message != "" {
		b.pcan.available = false
	}
	end := !b.pcan.available
	channel := b.pcan.channel
	if end {
		b.pcan.armed = false
		b.pcan.commands = nil
	}
	b.pcanMu.Unlock()
	if end && channel > 0 {
		b.recordings.EndSource(fmt.Sprintf("pcan:usb:%d", channel))
	}
	w.WriteHeader(204)
}
func (b *browserService) configurePCAN(c *browserClient, q browserControl) error {
	var params struct {
		Channel    int   `json:"channel"`
		Baud       int   `json:"baud"`
		ListenOnly *bool `json:"listen_only"`
	}
	_ = json.Unmarshal(q.Raw, &params)
	if params.Channel == 0 {
		params.Channel = 1
	}
	if params.Baud == 0 {
		params.Baud = 250000
	}
	if params.Channel < 1 || params.Channel > 8 {
		return fmt.Errorf("invalid PCAN channel")
	}
	switch params.Baud {
	case 125000, 250000, 500000, 1000000:
	default:
		return fmt.Errorf("invalid PCAN baud")
	}
	listen := true
	if params.ListenOnly != nil {
		listen = *params.ListenOnly
	}
	b.pcanMu.Lock()
	if b.pcan.owner != nil && b.pcan.owner != c {
		b.pcanMu.Unlock()
		return fmt.Errorf("PCAN is controlled by another client")
	}
	b.pcan.owner = c
	b.pcan.armed = false
	b.pcan.listenOnly = listen
	b.pcan.channel = params.Channel
	b.pcan.heartbeat = time.Now()
	b.pcan.available = false
	b.pcan.commands = []any{map[string]any{"type": "pcan_config", "channel": params.Channel, "baud": params.Baud, "listen_only": listen}}
	b.pcanMu.Unlock()
	c.pcanSource = fmt.Sprintf("pcan:usb:%d", params.Channel)
	c.lastHeartbeat = time.Now()
	c.mode = 3
	c.session = ""
	return c.ack()
}
func (b *browserService) releasePCAN(c *browserClient) {
	b.pcanMu.Lock()
	defer b.pcanMu.Unlock()
	if b.pcan.owner == c {
		b.pcan.armed = false
		b.pcan.owner = nil
		b.pcan.commands = []any{map[string]any{"type": "pcan_close"}}
	}
	c.pcanSource = ""
}
func (b *browserService) pcanCommand(c *browserClient, q browserControl) error {
	if q.Type == "tx_arm_request" {
		b.pcanMu.Lock()
		if b.pcan.owner != c {
			b.pcanMu.Unlock()
			return fmt.Errorf("select the local PCAN adapter first")
		}
		b.pcan.armed = q.Armed && b.pcan.available && !b.pcan.listenOnly && time.Since(b.pcan.seen) < 2*time.Second
		armed := b.pcan.armed
		b.pcanMu.Unlock()
		return c.sendJSON(map[string]any{"type": "tx_arm_status", "armed": armed, "reason": "PCAN adapter arming"})
	}
	b.pcanMu.Lock()
	defer b.pcanMu.Unlock()
	if b.pcan.owner != c {
		return fmt.Errorf("select the local PCAN adapter first")
	}
	if !b.pcan.armed || b.pcan.listenOnly || time.Since(c.lastHeartbeat) > 3*time.Second || time.Since(b.pcan.seen) > 2*time.Second {
		return fmt.Errorf("PCAN transmit is not armed")
	}
	if c.defs == nil {
		return fmt.Errorf("DBC required")
	}
	var tx struct {
		Message string `json:"message"`
		Signals []struct {
			Name  string  `json:"name"`
			Value float64 `json:"value"`
		} `json:"signals"`
	}
	if err := json.Unmarshal(q.Raw, &tx); err != nil {
		return err
	}
	var frame cache.Frame
	found := false
	for _, d := range c.defs.Definitions {
		if d.Message != tx.Message {
			continue
		}
		found = true
		frame.CANID = d.ID.CANID
		frame.DLC = int(d.DLC)
		if d.ID.Extended {
			frame.Flags = 1
		}
		for _, sig := range tx.Signals {
			if sig.Name != d.ID.Name {
				continue
			}
			if d.Factor == 0 {
				return fmt.Errorf("zero signal factor")
			}
			raw := (sig.Value - d.Offset) / d.Factor
			if math.IsNaN(raw) || math.IsInf(raw, 0) {
				return fmt.Errorf("non-finite transmit value")
			}
			var bits uint64
			if d.ValueType == 1 {
				bits = uint64(math.Float32bits(float32(raw)))
			} else if d.ValueType == 2 {
				bits = math.Float64bits(raw)
			} else {
				raw = math.Round(raw)
				limit := math.Ldexp(1, int(d.BitLength))
				if d.Signed {
					limit /= 2
					if raw < -limit || raw >= limit {
						return fmt.Errorf("value exceeds signed signal width")
					}
					bits = uint64(int64(raw))
				} else {
					if raw < 0 || raw >= limit {
						return fmt.Errorf("value exceeds unsigned signal width")
					}
					bits = uint64(raw)
				}
			}
			bit := int(d.StartBit)
			for i := 0; i < int(d.BitLength); i++ {
				shift := i
				if !d.LittleEndian {
					shift = int(d.BitLength) - 1 - i
				}
				mask := byte(1 << uint(bit%8))
				frame.Data[bit/8] &= ^mask
				if bits>>uint(shift)&1 != 0 {
					frame.Data[bit/8] |= mask
				}
				if d.LittleEndian {
					bit++
				} else if bit%8 == 0 {
					bit += 15
				} else {
					bit--
				}
			}
		}
	}
	if !found {
		return fmt.Errorf("unknown DBC message")
	}
	if len(b.pcan.commands) >= 16 {
		return fmt.Errorf("PCAN command queue full")
	}
	b.pcan.commands = append(b.pcan.commands, map[string]any{"type": "pcan_tx", "can_id": frame.CANID, "flags": frame.Flags, "dlc": frame.DLC, "data": hex.EncodeToString(frame.Data[:])})
	return nil
}
