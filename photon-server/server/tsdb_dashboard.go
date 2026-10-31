package main

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"os"
	"path/filepath"
	"sync"
	"time"
)

// historyQuerySlots is how many history queries one page may have in flight.
// The UI caps itself at 16 and never retries a query it already marked
// requested, including one the server rejected.
const historyQuerySlots = 64

// A cursor tile is one rollup span (about 17 minutes). Wider requests, such
// as a multi-hour export, stay as the caller asked so one pin cannot pull
// every sample in the run.
const historyExpandMaxWindow = 20 * 60 * 1000

// Cell taps are a mux. The page rebuilds them only from raw (level 0)
// samples, and a 2000-point budget rolls a whole tile up to 100 ms means.
const historyRawMaxPoints = 8000
const historyRawSliceMs = 180000

// historyBatchWait collects the pin burst (one websocket message per signal)
// into a single store read.
var historyBatchWait = 25 * time.Millisecond

// Names the dashboard maps in photon-next/src/ui/signal_map.cpp. A saved run
// paints gauges from history tiles, and the page never asks for most of these
// once its 16 in-flight slots are full. Carrying them on a query it did send
// fills those tiles, because the page files each chunk under its signal id.
const dashboardSignalText = `
BPS_Status.Main_Battery_Voltage
MC_BusMeasurement.MC_BusCurrent
BPS_Status.Main_Battery_Avg_Temperature
BPS_Status.BPS_Fault
BPS_Status.HV_Plus_Contactor_State
BPS_Status.HV_Minus_Contactor_State
BPS_Status.Array_Contactor_State
BPS_Status.Array_Precharge_Contactor_State
VCU_Status.Motor_Contactor_State
VCU_Status.Motor_Precharge_Contactor_State
MC_Motor_TempMeasurement.MC_MotorTemp
MC_Motor_TempMeasurement.MC_HeatsinkTemp
MC_VelocityMeasurement.MC_MotorVelocity
MC_VelocityMeasurement.MC_VehicleVelocity
Pedal_Status.AccelPedal_Main_Pos
Pedal_Status.BrakePedal_Main_Pos
Driver_Input_Status.Gear_Forward
Driver_Input_Status.Gear_Neutral
Driver_Input_Status.Gear_Reverse
Driver_Input_Status.Regen_Enable
Driver_Input_Status.Regen_Activate
Driver_Input_Status.Cruise_Enable
Driver_Input_Status.Blinker_Left
Driver_Input_Status.Blinker_Right
Driver_Input_Status.Hazard_Pressed
MPPT_A_PowerMeasurements.MPPT_Input_Voltage
MPPT_A_PowerMeasurements.MPPT_Input_Current
MPPT_B_PowerMeasurements.MPPT_Input_Voltage
MPPT_B_PowerMeasurements.MPPT_Input_Current
MPPT_C_PowerMeasurements.MPPT_Input_Voltage
MPPT_C_PowerMeasurements.MPPT_Input_Current
Supp_Battery_Status.Supplemental_Battery_Voltage
Supp_Battery_Status.Supplemental_Battery_Current
VCU_Status.Motor_Ready
Driver_Input_Status.Ignition_Array
Driver_Input_Status.Ignition_Motor
Driver_Input_Status.Ignition_Off
BPS_Status.BPS_Charge_OK
BPS_Status.BPS_Regen_OK
VCU_Status.VCU_BPS_FAULT_DETECTED
VCU_Status.VCU_CONTROLS_FAULT_DETECTED
VCU_Status.VCU_MTR_FAULT_DETECTED
VCU_Status.VCU_PEDALS_FAULT_DETECTED
VCU_Status.VCU_STEERING_FAULT_DETECTED
VCU_Status.VCU_OTHER_FAULT
MPPT_A_Status.MPPT_Fault
MPPT_B_Status.MPPT_Fault
MPPT_C_Status.MPPT_Fault
LWS_Standard.LWS_Angle
WheelRPM_FL.RPM
WheelRPM_FR.RPM
Suspension_FL.SUS_Data1
Suspension_FR.SUS_Data1
Suspension_Back.SUS_Data1
Lattitude_GPS.Latitude
Longitude_GPS.Longitude
Speed_GPS.Speed
Valid_GPS.Valid
IMU_B_Accel.Accel_X
IMU_B_Accel.Accel_Y
IMU_Accel_FR.Accel_X
IMU_Accel_FR.Accel_Y
IMU_Accel_FL.Accel_X
IMU_Accel_FL.Accel_Y
IMU_Gyro_Back.Gyro_X
IMU_Gyro_Back.Gyro_Y
IMU_Gyro_Back.Gyro_Z
LWS_Standard.LWS_Fault
MC_BusMeasurement.MC_BusVoltage
MC_DriveCommand.MC_MotorVelocitySetpoint
MC_DriveCommand.MC_MotorCurrentSetpoint
MC_PowerCommand.MC_MotorPowerSetpoint
VCU_Precharge_Voltages.VCU_Precharge_Motor_Voltage
VCU_Precharge_Voltages.VCU_Precharge_Battery_Voltage
MC_Status.MC_LIMIT_OutputVoltagePWM
MC_Status.MC_LIMIT_MotorCurrent
MC_Status.MC_LIMIT_Velocity
MC_Status.MC_LIMIT_BusCurrent
MC_Status.MC_LIMIT_BusVoltageUpper
MC_Status.MC_LIMIT_BusVoltageLower
MC_Status.MC_LIMIT_MotorTemp
MC_Status.MC_FAULT_HardwareOverCurrent
MC_Status.MC_FAULT_SoftwareOverCurrent
MC_Status.MC_FAULT_DcBusOverVoltage
MC_Status.MC_FAULT_BadMotorPositionHallSeq
MC_Status.MC_FAULT_WatchdogCausedLastReset
MC_Status.MC_FAULT_ConfigRead
MC_Status.MC_FAULT_15vRailUnderVoltage
MC_Status.MC_FAULT_DesaturationFault
MC_Status.MC_FAULT_MotorOverSpeed
Brake_Pressure_1.Brake_Pressure
Brake_Pressure_2.Brake_Pressure
Pedal_Status.Brake_Pressure_1_Fault
Pedal_Status.Brake_Pressure_2_Fault
Pedal_Status.AccelPedal_Redundant_Pos
Pedal_Status.BrakePedal_Redundant_Pos
Pedal_Status.AccelPedal_Main_Fault
Pedal_Status.AccelPedal_Redundant_Fault
Pedal_Status.BrakePedal_Main_Fault
Pedal_Status.BrakePedal_Redundant_Fault
Driver_Input_Status.Horn_Pressed
Driver_Input_Status.PushToTalk_Pressed
Driver_Input_Status.Cruise_Set
VCU_Status.VCU_FSM_State
VCU_Status.VCU_Pedals_Watchdog
VCU_Status.VCU_Driver_Input_Watchdog
VCU_Status.VCU_Regen_OK
VCU_Status.VCU_Regen_Active
MPPT_A_PowerMeasurements.MPPT_Output_Voltage
MPPT_A_PowerMeasurements.MPPT_Output_Current
MPPT_A_Status.MPPT_HeatsinkTemperature
MPPT_A_Status.MPPT_AmbientTemperature
MPPT_A_Status.MPPT_Mode
Coolant_Temperature.Coolant_Temperature_1
Coolant_Temperature.Coolant_Temperature_2
Pump_Status.FlowRate_1
Pump_Status.FlowRate_2
Pump_Status.Pump_DutyCycle
Pump_Status.Pump_Fault
LV_Carrier_Status.LTC4421_HVDCDC_Selected
LV_Carrier_Status.LTC4421_HVDCDC_Fault
LV_Carrier_Status.LTC4421_HVDCDC_Valid
LV_Carrier_Status.LTC4421_SuppBatt_Selected
LV_Carrier_Status.LTC4421_SuppBatt_Fault
LV_Carrier_Status.LTC4421_SuppBatt_Valid
LV_Carrier_Status.LV_EN_SupplementalBattery
LV_Carrier_Status.LV_EN_PowerSupply
Display_Status.Camera_Status_Backup
Display_Status.Camera_Status_Left
Display_Status.Camera_Status_Right
Controls_Status.Controls_Leader_Fault
Controls_Status.LightingBoard_Front_Status
Controls_Status.LightingBoard_Left_Status
Controls_Status.LightingBoard_Right_Status
Controls_Status.LightingBoard_Rear_Status
Controls_Status.LightingBoard_Canopy_Status
MC_PhaseCurrentMeasurement.MC_PhaseCurrentB
MC_PhaseCurrentMeasurement.MC_PhaseCurrentC
MC_BackEMFMeasurementPrediction.MC_BEMFq
MC_BackEMFMeasurementPrediction.MC_BEMFd
Supp_Charger_Status.Supplemental_Charger_Status
Supp_Vicor_Stats.Supplemental_Vicor_Voltage
Supp_Vicor_Stats.Supplemental_Vicor_Current
BPS_Voltage_Aggregate_Arr.BPS_Tap_idx
BPS_Voltage_Aggregate_Arr.BPS_Voltage_Tap_Data
BPS_Temperature_Aggregate_Arr.BPS_Tap_idx
BPS_Temperature_Aggregate_Arr.BPS_Temperature_Tap_Data
`

const historyChunkFinal uint16 = 1

type historyRunner func(ctx context.Context, queryID uint32, from, to uint64, max int, ids []uint32) (historyHeader, [][]byte, error)

type historyBatchKey struct {
	dir      string
	from, to uint64
	max      int
}

type historyStored struct {
	at     time.Time
	header historyHeader
	chunks [][]byte
}

type historyWaiter struct {
	id    uint32
	asked []uint32
}

type historyDelivery struct {
	header historyHeader
	chunks [][]byte
}

type historyJob struct {
	q      browserControl
	ctx    context.Context
	result chan historyDelivery
	errc   chan error
}

type historyBatch struct {
	key    historyBatchKey
	mu     sync.Mutex
	jobs   []*historyJob
	armed  bool
	closed bool
}

var historyBatches = struct {
	sync.Mutex
	m map[historyBatchKey]*historyBatch
}{m: map[historyBatchKey]*historyBatch{}}

var historyResultCache = struct {
	sync.Mutex
	order []historyBatchKey
	m     map[historyBatchKey]historyStored
}{m: map[historyBatchKey]historyStored{}}

var dashboardNames = map[string]bool{}
var dashboardNamesOnce sync.Once

func loadDashboardNames() map[string]bool {
	dashboardNamesOnce.Do(func() {
		for _, line := range splitLines(dashboardSignalText) {
			dashboardNames[line] = true
		}
	})
	return dashboardNames
}

func splitLines(text string) []string {
	var out []string
	start := 0
	for i := 0; i <= len(text); i++ {
		if i == len(text) || text[i] == '\n' {
			line := text[start:i]
			start = i + 1
			if len(line) > 0 && line[len(line)-1] == '\r' {
				line = line[:len(line)-1]
			}
			if line != "" {
				out = append(out, line)
			}
		}
	}
	return out
}

func isRawDashboardSignal(name string) bool {
	switch name {
	case "BPS_Voltage_Aggregate_Arr.BPS_Tap_idx",
		"BPS_Voltage_Aggregate_Arr.BPS_Voltage_Tap_Data",
		"BPS_Temperature_Aggregate_Arr.BPS_Tap_idx",
		"BPS_Temperature_Aggregate_Arr.BPS_Temperature_Tap_Data":
		return true
	default:
		return false
	}
}

type dashboardIDs struct {
	mod time.Time
	all []uint32
	raw []uint32
}

var dashboardIDCache sync.Map

func dashboardSignals(dir string) (all, raw []uint32) {
	path := filepath.Join(dir, "signals.json")
	st, err := os.Stat(path)
	if err != nil {
		return nil, nil
	}
	if v, ok := dashboardIDCache.Load(dir); ok {
		cached := v.(dashboardIDs)
		if cached.mod.Equal(st.ModTime()) {
			return cached.all, cached.raw
		}
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, nil
	}
	var rows []struct {
		ID      uint32 `json:"id"`
		Message string `json:"message"`
		Name    string `json:"name"`
	}
	if json.Unmarshal(data, &rows) != nil {
		return nil, nil
	}
	names := loadDashboardNames()
	var got dashboardIDs
	got.mod = st.ModTime()
	for _, row := range rows {
		if row.ID == 0 || row.Name == "" {
			continue
		}
		key := row.Message + "." + row.Name
		if !names[key] {
			continue
		}
		got.all = append(got.all, row.ID)
		if isRawDashboardSignal(key) {
			got.raw = append(got.raw, row.ID)
		}
	}
	dashboardIDCache.Store(dir, got)
	return got.all, got.raw
}

func limitIDs(ids []uint32, n int) []uint32 {
	if len(ids) <= n {
		return ids
	}
	return append([]uint32(nil), ids[:n]...)
}

func unionIDs(base, extra []uint32, n int) []uint32 {
	seen := make(map[uint32]bool, len(base)+len(extra))
	out := make([]uint32, 0, len(base)+len(extra))
	add := func(id uint32) {
		if id == 0 || seen[id] || len(out) >= n {
			return
		}
		seen[id] = true
		out = append(out, id)
	}
	for _, id := range base {
		add(id)
	}
	for _, id := range extra {
		add(id)
	}
	return out
}

func expandWindow(from, to uint64) bool {
	return to >= from && to-from <= historyExpandMaxWindow
}

func chunkSignalID(b []byte) (uint32, bool) {
	if len(b) < 8 {
		return 0, false
	}
	return binary.LittleEndian.Uint32(b[4:8]), true
}

func rewriteChunk(b []byte, queryID uint32, final bool) []byte {
	out := append([]byte(nil), b...)
	if len(out) < 12 {
		return out
	}
	binary.LittleEndian.PutUint32(out[0:4], queryID)
	flags := binary.LittleEndian.Uint16(out[10:12])
	if final {
		flags |= historyChunkFinal
	} else {
		flags &^= historyChunkFinal
	}
	binary.LittleEndian.PutUint16(out[10:12], flags)
	return out
}

func fanoutHistory(header historyHeader, chunks [][]byte, waiters []historyWaiter, extras bool) []historyDelivery {
	asked := map[uint32]bool{}
	for _, w := range waiters {
		for _, id := range w.asked {
			if id != 0 {
				asked[id] = true
			}
		}
	}
	by := map[uint32][][]byte{}
	var extraChunks [][]byte
	for _, chunk := range chunks {
		sid, ok := chunkSignalID(chunk)
		if !ok {
			continue
		}
		if asked[sid] {
			by[sid] = append(by[sid], chunk)
			continue
		}
		if extras {
			extraChunks = append(extraChunks, chunk)
		}
	}
	out := make([]historyDelivery, len(waiters))
	for i, w := range waiters {
		var mine [][]byte
		if extras && i == 0 {
			mine = append(mine, extraChunks...)
		}
		for _, id := range w.asked {
			mine = append(mine, by[id]...)
		}
		patched := make([][]byte, len(mine))
		for j, chunk := range mine {
			patched[j] = rewriteChunk(chunk, w.id, j == len(mine)-1)
		}
		out[i] = historyDelivery{header: header, chunks: patched}
	}
	return out
}

func seriesLevel(h historyHeader, id uint32) (int, bool) {
	var level int
	found := false
	for _, s := range h.Series {
		if s.SignalID != id {
			continue
		}
		if !found || s.Level < level {
			level = s.Level
			found = true
		}
	}
	return level, found
}

func coarseIDs(h historyHeader, ids []uint32) []uint32 {
	var out []uint32
	for _, id := range ids {
		level, ok := seriesLevel(h, id)
		if !ok || level != 0 {
			out = append(out, id)
		}
	}
	return out
}

func chunksFor(chunks [][]byte, ids []uint32) [][]byte {
	want := map[uint32]bool{}
	for _, id := range ids {
		want[id] = true
	}
	var out [][]byte
	for _, chunk := range chunks {
		sid, ok := chunkSignalID(chunk)
		if ok && want[sid] {
			out = append(out, chunk)
		}
	}
	return out
}

func dropChunks(chunks [][]byte, ids []uint32) [][]byte {
	drop := map[uint32]bool{}
	for _, id := range ids {
		drop[id] = true
	}
	var out [][]byte
	for _, chunk := range chunks {
		sid, ok := chunkSignalID(chunk)
		if ok && drop[sid] {
			continue
		}
		out = append(out, chunk)
	}
	return out
}

// fetchRawLevels re-reads mux signals until the store returns raw samples.
// A wide tile at the page's 2000-point budget is a rollup, and the cell grid
// ignores those. The whole tile is tried first; a long run is then read in
// 180 s slices, which stays under the raw-sample budget at the pack's rate.
func fetchRawLevels(ctx context.Context, run historyRunner, queryID uint32, from, to uint64, ids []uint32) (historyHeader, [][]byte, error) {
	var zero historyHeader
	if len(ids) == 0 || to < from {
		return zero, nil, nil
	}
	h, chunks, err := run(ctx, queryID, from, to, historyRawMaxPoints, ids)
	if err != nil {
		return zero, nil, err
	}
	need := coarseIDs(h, ids)
	keptH, keptC := filterSeries(h, chunks, minusIDs(ids, need))
	if len(need) == 0 || to-from <= historyRawSliceMs {
		return keptH, keptC, nil
	}
	for start := from; start < to; start += historyRawSliceMs {
		end := start + historyRawSliceMs
		if end > to {
			end = to
		}
		sh, sc, serr := run(ctx, queryID, start, end, historyRawMaxPoints, need)
		if serr != nil {
			return keptH, keptC, nil
		}
		got := minusIDs(need, coarseIDs(sh, need))
		partH, partC := filterSeries(sh, sc, got)
		keptH, keptC = mergeHistory(keptH, keptC, partH, partC)
		still := coarseIDs(sh, need)
		if len(still) == 0 || end-start <= 15000 {
			continue
		}
		mid := start + (end-start)/2
		for _, piece := range [][2]uint64{{start, mid}, {mid, end}} {
			ph, pc, perr := run(ctx, queryID, piece[0], piece[1], historyRawMaxPoints, still)
			if perr != nil {
				continue
			}
			got = minusIDs(still, coarseIDs(ph, still))
			partH, partC = filterSeries(ph, pc, got)
			keptH, keptC = mergeHistory(keptH, keptC, partH, partC)
		}
	}
	return keptH, keptC, nil
}

func minusIDs(ids, drop []uint32) []uint32 {
	skip := map[uint32]bool{}
	for _, id := range drop {
		skip[id] = true
	}
	var out []uint32
	for _, id := range ids {
		if !skip[id] {
			out = append(out, id)
		}
	}
	return out
}

func filterSeries(h historyHeader, chunks [][]byte, ids []uint32) (historyHeader, [][]byte) {
	want := map[uint32]bool{}
	for _, id := range ids {
		want[id] = true
	}
	var series []historySeriesMeta
	for _, s := range h.Series {
		if want[s.SignalID] && s.Level == 0 {
			series = append(series, s)
		}
	}
	h.Series = series
	return h, chunksFor(chunks, ids)
}

func mergeHistory(a historyHeader, aChunks [][]byte, b historyHeader, bChunks [][]byte) (historyHeader, [][]byte) {
	if len(b.Series) == 0 && len(bChunks) == 0 {
		return a, aChunks
	}
	a.Series = append(a.Series, b.Series...)
	a.Chunks += b.Chunks
	return a, append(aChunks, bChunks...)
}

func overlayRawMux(ctx context.Context, run historyRunner, queryID uint32, from, to uint64, header historyHeader, chunks [][]byte, rawIDs []uint32) (historyHeader, [][]byte) {
	need := coarseIDs(header, rawIDs)
	if len(need) == 0 {
		return header, chunks
	}
	rawH, rawC, err := fetchRawLevels(ctx, run, queryID, from, to, need)
	if err != nil || len(rawC) == 0 {
		return header, chunks
	}
	got := make([]uint32, 0, len(need))
	seen := map[uint32]bool{}
	for _, chunk := range rawC {
		sid, ok := chunkSignalID(chunk)
		if ok && !seen[sid] {
			seen[sid] = true
			got = append(got, sid)
		}
	}
	chunks = dropChunks(chunks, got)
	header.Series = append(dropSeries(header.Series, got), rawH.Series...)
	header.Chunks = len(chunks) + len(rawC)
	return header, append(chunks, rawC...)
}

func dropSeries(series []historySeriesMeta, ids []uint32) []historySeriesMeta {
	drop := map[uint32]bool{}
	for _, id := range ids {
		drop[id] = true
	}
	var out []historySeriesMeta
	for _, s := range series {
		if !drop[s.SignalID] {
			out = append(out, s)
		}
	}
	return out
}

func historyCacheGet(key historyBatchKey, ids []uint32) (historyStored, bool) {
	historyResultCache.Lock()
	defer historyResultCache.Unlock()
	stored, ok := historyResultCache.m[key]
	if !ok || time.Since(stored.at) > 30*time.Second {
		return historyStored{}, false
	}
	have := map[uint32]bool{}
	for _, s := range stored.header.Series {
		have[s.SignalID] = true
	}
	for _, chunk := range stored.chunks {
		if sid, ok := chunkSignalID(chunk); ok {
			have[sid] = true
		}
	}
	for _, id := range ids {
		if !have[id] {
			return historyStored{}, false
		}
	}
	return stored, true
}

func historyCachePut(key historyBatchKey, stored historyStored) {
	historyResultCache.Lock()
	defer historyResultCache.Unlock()
	if _, ok := historyResultCache.m[key]; !ok {
		historyResultCache.order = append(historyResultCache.order, key)
	}
	stored.at = time.Now()
	historyResultCache.m[key] = stored
	for len(historyResultCache.order) > 32 {
		old := historyResultCache.order[0]
		historyResultCache.order = historyResultCache.order[1:]
		delete(historyResultCache.m, old)
	}
}

func resetHistoryBatches() {
	historyBatches.Lock()
	historyBatches.m = map[historyBatchKey]*historyBatch{}
	historyBatches.Unlock()
	historyResultCache.Lock()
	historyResultCache.order = nil
	historyResultCache.m = map[historyBatchKey]historyStored{}
	historyResultCache.Unlock()
}

func coalesceHistory(ctx context.Context, dir string, q browserControl, run historyRunner) (historyHeader, [][]byte, error) {
	var zero historyHeader
	key := historyBatchKey{dir: dir, from: q.From, to: q.To, max: q.MaxPoints}
	if stored, ok := historyCacheGet(key, q.SignalIDs); ok {
		d := fanoutHistory(stored.header, stored.chunks, []historyWaiter{{id: q.QueryID, asked: append([]uint32(nil), q.SignalIDs...)}}, false)
		return d[0].header, d[0].chunks, nil
	}
	job := &historyJob{q: q, ctx: ctx, result: make(chan historyDelivery, 1), errc: make(chan error, 1)}
	if err := enqueueHistory(key, job, run); err != nil {
		return zero, nil, err
	}
	select {
	case d := <-job.result:
		return d.header, d.chunks, nil
	case err := <-job.errc:
		return zero, nil, err
	case <-ctx.Done():
		return zero, nil, ctx.Err()
	}
}

func enqueueHistory(key historyBatchKey, job *historyJob, run historyRunner) error {
	for {
		historyBatches.Lock()
		b := historyBatches.m[key]
		if b == nil {
			b = &historyBatch{key: key}
			historyBatches.m[key] = b
		}
		b.mu.Lock()
		historyBatches.Unlock()
		if b.closed {
			b.mu.Unlock()
			continue
		}
		b.jobs = append(b.jobs, job)
		if !b.armed {
			b.armed = true
			go b.run(run)
		}
		b.mu.Unlock()
		return nil
	}
}

func (b *historyBatch) run(run historyRunner) {
	timer := time.NewTimer(historyBatchWait)
	defer timer.Stop()
	<-timer.C
	b.mu.Lock()
	jobs := b.jobs
	b.jobs = nil
	b.closed = true
	b.mu.Unlock()
	historyBatches.Lock()
	if historyBatches.m[b.key] == b {
		delete(historyBatches.m, b.key)
	}
	historyBatches.Unlock()

	alive := make([]*historyJob, 0, len(jobs))
	for _, job := range jobs {
		select {
		case <-job.ctx.Done():
			job.errc <- job.ctx.Err()
		default:
			alive = append(alive, job)
		}
	}
	if len(alive) == 0 {
		return
	}
	from, to := b.key.from, b.key.to
	max := b.key.max
	if max <= 0 || max > 8000 {
		max = 8000
	}
	var asked []uint32
	waiters := make([]historyWaiter, len(alive))
	for i, job := range alive {
		asked = unionIDs(asked, job.q.SignalIDs, 512)
		waiters[i] = historyWaiter{id: job.q.QueryID, asked: append([]uint32(nil), job.q.SignalIDs...)}
	}
	var rawIDs []uint32
	if expandWindow(from, to) {
		dash, raw := dashboardSignals(b.key.dir)
		asked = unionIDs(asked, dash, 512)
		rawIDs = raw
	}
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	header, chunks, err := run(ctx, alive[0].q.QueryID, from, to, max, asked)
	if err != nil {
		for _, job := range alive {
			job.errc <- err
		}
		return
	}
	if expandWindow(from, to) && len(rawIDs) > 0 {
		header, chunks = overlayRawMux(ctx, run, alive[0].q.QueryID, from, to, header, chunks, rawIDs)
	}
	historyCachePut(b.key, historyStored{header: header, chunks: chunks})
	delivered := fanoutHistory(header, chunks, waiters, true)
	for i, job := range alive {
		job.result <- delivered[i]
	}
}
