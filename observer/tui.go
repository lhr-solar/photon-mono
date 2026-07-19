package main

import (
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"image/color"
	"net"
	"os"
	"sort"
	"strconv"
	"strings"
	"time"

	"charm.land/bubbles/v2/progress"
	"charm.land/bubbles/v2/table"
	"charm.land/bubbles/v2/textinput"
	"charm.land/bubbles/v2/viewport"
	tea "charm.land/bubbletea/v2"
	"charm.land/lipgloss/v2"
	tslc "github.com/NimbleMarkets/ntcharts/v2/linechart/timeserieslinechart"
)

type metadata struct {
	StartTime   time.Time `json:"start_time"`
	UptimeMS    int64     `json:"uptime_ms"`
	CurrentTime time.Time `json:"current_time"`
	PID         int       `json:"pid"`
	Goroutines  int       `json:"goroutines"`
	MemoryBytes uint64    `json:"memory_bytes"`
	Ingress     ingress   `json:"ingress"`
	Clients     clients   `json:"clients"`
	Queue       queue     `json:"queue"`
	Storage     storage   `json:"storage"`
}

type ingress struct {
	Listening      bool   `json:"listening"`
	Active         int64  `json:"active"`
	Total          uint64 `json:"total"`
	Bytes          uint64 `json:"bytes"`
	Reads          uint64 `json:"reads"`
	LastReceiveMS  int64  `json:"last_receive_ms"`
	ReadFailures   uint64 `json:"read_failures"`
	AcceptFailures uint64 `json:"accept_failures"`
}

type client struct {
	ID            string `json:"id"`
	Remote        string `json:"remote"`
	ConnectedMS   int64  `json:"connected_ms"`
	NextID        uint64 `json:"next_id"`
	Lag           uint64 `json:"lag"`
	Bytes         uint64 `json:"bytes"`
	Messages      uint64 `json:"messages"`
	LastSendMS    int64  `json:"last_send_ms"`
	WriteFailures uint64 `json:"write_failures"`
}

type clients struct {
	Active         int      `json:"active"`
	Total          uint64   `json:"total"`
	Bytes          uint64   `json:"bytes"`
	Messages       uint64   `json:"messages"`
	MaxLag         uint64   `json:"max_lag"`
	LastSendMS     int64    `json:"last_send_ms"`
	WriteFailures  uint64   `json:"write_failures"`
	AcceptFailures uint64   `json:"accept_failures"`
	Clients        []client `json:"clients"`
}

type queue struct {
	Depth     int    `json:"depth"`
	Capacity  int    `json:"capacity"`
	PeakDepth uint64 `json:"peak_depth"`
	BlockedMS int64  `json:"blocked_ms"`
}

type storedPacket struct {
	Key       string
	Timestamp uint64
	Seq       uint32
	Index     uint16
	CANID     uint32
	DLC       uint8
	Data      string
	Deltas    [8]uint16
}

type storedChunk struct {
	ID       uint64 `json:"id"`
	Bytes    int    `json:"bytes"`
	Captured int    `json:"captured"`
	Data     []byte `json:"data"`
}

type storage struct {
	OldestID          uint64         `json:"oldest_id"`
	LatestID          uint64         `json:"latest_id"`
	RetainedMessages  uint64         `json:"retained_messages"`
	Segments          int            `json:"segments"`
	ActiveSegmentID   uint64         `json:"active_segment_id"`
	ActiveSegmentSize int64          `json:"active_segment_size"`
	DiskBytes         int64          `json:"disk_bytes"`
	HotBytes          int64          `json:"hot_bytes"`
	PressureHigh      bool           `json:"pressure_high"`
	Chunks            []storedChunk  `json:"chunks"`
	Packets           []storedPacket `json:"-"`
}

type clientRate struct {
	bytes, messages uint64
	byteRate, rate  float64
	prevByteRate    float64
	generation      uint64
	points          [60]tslc.TimePoint
	at, len         int
}

type packetSortMode uint8

const (
	sortNewest packetSortMode = iota
	sortCANIDAscending
	sortCANIDDescending
)

type model struct {
	decoder                 *json.Decoder
	data                    metadata
	err                     error
	width                   int
	height                  int
	dot                     bool
	byteRate                float64
	readRate                float64
	prevByteRate            float64
	prevReadRate            float64
	chart                   tslc.Model
	clientChart             tslc.Model
	queueChart              tslc.Model
	queueProgress           progress.Model
	queueGrowth             int
	queueRates              [120]tslc.TimePoint
	queueRateAt             int
	queueRateLen            int
	rates                   [60]tslc.TimePoint
	rateAt                  int
	rateLen                 int
	clientRates             map[string]*clientRate
	clientGeneration        uint64
	clientByteRate          float64
	clientMessageRate       float64
	prevClientByteRate      float64
	prevClientMessageRate   float64
	clientTable             table.Model
	packetTable             table.Model
	packetKeys              []string
	selectedPacketKey       string
	packetHistory           []storedPacket
	packetSeen              map[string]struct{}
	packetRate              float64
	prevPacketRate          float64
	packetSort              packetSortMode
	filterInput             textinput.Model
	filtering               bool
	filterCANID             uint32
	filterActive            bool
	page                    viewport.Model
	ingressHorizontal       bool
	clientsHorizontal       bool
	clientsTwoRows          bool
	packetDetailsHorizontal bool
	packetDetailsWidth      int
	queueStorageHorizontal  bool
	focus                   int
}

var (
	green     = lipgloss.Color("#A8D5BA")
	yellow    = lipgloss.Color("#E6D39A")
	red       = lipgloss.Color("#E6A6A6")
	blue      = lipgloss.Color("#9EC5E6")
	white     = lipgloss.Color("#F2F0EB")
	softWhite = lipgloss.Color("#C9C7C2")
	text      = lipgloss.Color("#D8DEE9")
	dim       = lipgloss.Color("#737A86")
	title     = lipgloss.NewStyle().Bold(true).Foreground(white)
	label     = lipgloss.NewStyle().Width(12).Foreground(dim)
	value     = lipgloss.NewStyle().Foreground(text)
	muted     = lipgloss.NewStyle().Foreground(dim)
	panel     = lipgloss.NewStyle().
			Border(lipgloss.RoundedBorder()).
			BorderForeground(softWhite).
			Padding(0, 1)
)

const metricColumnGap = "  "

type blinkMsg time.Time

func blink() tea.Cmd {
	return tea.Tick(time.Second, func(t time.Time) tea.Msg { return blinkMsg(t) })
}

func read(decoder *json.Decoder) tea.Cmd {
	return func() tea.Msg {
		var data metadata
		if err := decoder.Decode(&data); err != nil {
			return err
		}
		return data
	}
}

func (m model) Init() tea.Cmd { return tea.Batch(read(m.decoder), blink()) }

func (m model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	var filterCmd tea.Cmd
	if m.filtering {
		if key, ok := msg.(tea.KeyPressMsg); ok {
			switch key.String() {
			case "enter":
				m.filtering = false
				m.filterInput.Blur()
				m.applyFilter()
				m.setStorageRows()
				return m, nil
			case "esc":
				m.filtering = false
				m.filterInput.Blur()
				m.filterInput.SetValue("")
				m.filterActive = false
				m.setStorageRows()
				return m, nil
			}
			m.filterInput, filterCmd = m.filterInput.Update(msg)
			m.applyFilter()
			m.setStorageRows()
			return m, filterCmd
		}
		m.filterInput, filterCmd = m.filterInput.Update(msg)
	}

	switch msg := msg.(type) {
	case tea.KeyPressMsg:
		if msg.String() == "q" || msg.String() == "ctrl+c" {
			return m, tea.Quit
		}
		if msg.String() == "tab" {
			m.focus = (m.focus + 1) % 2
			m.setFocus()
			return m, nil
		}
		if msg.String() == "shift+tab" {
			m.focus = (m.focus + 1) % 2
			m.setFocus()
			return m, nil
		}
		if msg.String() == "/" {
			m.filtering = true
			m.filterInput.SetValue("")
			return m, m.filterInput.Focus()
		}
		if msg.String() == "s" && m.focus == 1 {
			m.packetSort = (m.packetSort + 1) % 3
			m.setStorageRows()
			return m, nil
		}
		if msg.String() == "ctrl+up" {
			m.page.ScrollUp(3)
		}
		if msg.String() == "ctrl+down" {
			m.page.ScrollDown(3)
		}
	case tea.WindowSizeMsg:
		m.width, m.height = msg.Width, msg.Height
		m.resize()
		m.drawRateChart()
	case blinkMsg:
		m.dot = !m.dot
		m.setStorageRows()
		return m, tea.Batch(blink(), filterCmd)
	case metadata:
		decodeStorage(&msg.Storage)
		seconds := 0.0
		if !m.data.CurrentTime.IsZero() {
			seconds = msg.CurrentTime.Sub(m.data.CurrentTime).Seconds()
			if seconds > 0 && msg.Ingress.Bytes >= m.data.Ingress.Bytes &&
				msg.Ingress.Reads >= m.data.Ingress.Reads {
				m.prevByteRate, m.prevReadRate = m.byteRate, m.readRate
				m.byteRate = float64(msg.Ingress.Bytes-m.data.Ingress.Bytes) / seconds
				m.readRate = float64(msg.Ingress.Reads-m.data.Ingress.Reads) / seconds
				m.rates[m.rateAt] = tslc.TimePoint{Time: msg.CurrentTime, Value: m.byteRate / 1024}
				m.rateAt = (m.rateAt + 1) % len(m.rates)
				if m.rateLen < len(m.rates) {
					m.rateLen++
				}
			}
			if seconds > 0 && msg.Clients.Bytes >= m.data.Clients.Bytes &&
				msg.Clients.Messages >= m.data.Clients.Messages {
				m.prevClientByteRate = m.clientByteRate
				m.prevClientMessageRate = m.clientMessageRate
				m.clientByteRate = float64(msg.Clients.Bytes-m.data.Clients.Bytes) / seconds
				m.clientMessageRate = float64(msg.Clients.Messages-m.data.Clients.Messages) / seconds
			}
		}
		m.clientGeneration++
		for _, client := range msg.Clients.Clients {
			rate, exists := m.clientRates[client.ID]
			if !exists {
				rate = &clientRate{}
				m.clientRates[client.ID] = rate
			}
			if exists && seconds > 0 && client.Bytes >= rate.bytes && client.Messages >= rate.messages {
				rate.prevByteRate = rate.byteRate
				rate.byteRate = float64(client.Bytes-rate.bytes) / seconds
				rate.rate = float64(client.Messages-rate.messages) / seconds
				rate.points[rate.at] = tslc.TimePoint{Time: msg.CurrentTime, Value: rate.byteRate / 1024}
				rate.at = (rate.at + 1) % len(rate.points)
				if rate.len < len(rate.points) {
					rate.len++
				}
			}
			rate.bytes, rate.messages = client.Bytes, client.Messages
			rate.generation = m.clientGeneration
		}
		for id, rate := range m.clientRates {
			if rate.generation != m.clientGeneration {
				delete(m.clientRates, id)
			}
		}
		m.queueGrowth = msg.Queue.Depth - m.data.Queue.Depth
		m.data = msg
		newPackets := m.mergePackets(msg.Storage.Packets)
		if seconds > 0 {
			m.prevPacketRate = m.packetRate
			m.packetRate = float64(newPackets) / seconds
		}
		m.resizeChart()
		m.drawRateChart()
		m.setClientRows()
		m.setStorageRows()
		m.resizeClientLayout()
		m.drawClientChart()
		m.resizeQueueBar()
		percent := 0.0
		if msg.Queue.Capacity > 0 {
			percent = float64(msg.Queue.Depth) / float64(msg.Queue.Capacity)
		}
		m.queueRates[m.queueRateAt] = tslc.TimePoint{
			Time:  msg.CurrentTime,
			Value: percent * 100,
		}
		m.queueRateAt = (m.queueRateAt + 1) % len(m.queueRates)
		if m.queueRateLen < len(m.queueRates) {
			m.queueRateLen++
		}
		m.queueProgress.FullColor = utilizationColor(percent)
		return m, tea.Batch(read(m.decoder), m.queueProgress.SetPercent(percent), filterCmd)
	case error:
		m.err = msg
	}
	var cmd tea.Cmd
	switch m.focus {
	case 0:
		m.clientTable, cmd = m.clientTable.Update(msg)
	case 1:
		m.packetTable, cmd = m.packetTable.Update(msg)
		m.rememberSelectedPacket()
	}
	var progressCmd tea.Cmd
	m.queueProgress, progressCmd = m.queueProgress.Update(msg)
	m.resizeClientLayout()
	m.drawClientChart()
	if _, ok := msg.(tea.MouseMsg); ok {
		m.page, _ = m.page.Update(msg)
	}
	return m, tea.Batch(cmd, progressCmd, filterCmd)
}

func blockContentWidth(outerWidth int) int {
	return max(1, outerWidth-panel.GetHorizontalFrameSize())
}

func renderBlock(outerWidth int, body string) string {
	contentWidth := blockContentWidth(outerWidth)
	body = lipgloss.NewStyle().MaxWidth(contentWidth).Render(body)
	return panel.Width(outerWidth).Render(body)
}

func renderBlockHeight(outerWidth, outerHeight int, body string) string {
	contentWidth := blockContentWidth(outerWidth)
	body = lipgloss.NewStyle().MaxWidth(contentWidth).Render(body)
	return panel.Width(outerWidth).Height(outerHeight).Render(body)
}

func (m *model) resize() {
	w, h := max(24, m.width), max(5, m.height)
	m.page.SetWidth(w)
	m.page.SetHeight(max(1, h-1))
	contentWidth := blockContentWidth(w)
	m.queueStorageHorizontal = w >= 96
	storageOuterWidth := w
	if m.queueStorageHorizontal {
		storageOuterWidth = w - 35
	}
	storageContentWidth := blockContentWidth(storageOuterWidth)
	m.packetDetailsHorizontal = storageContentWidth >= 124
	m.packetDetailsWidth = 0

	m.resizeChart()
	m.resizeQueueBar()

	listWidth := min(28, max(16, contentWidth/5))
	m.clientTable.SetHeight(max(4, min(14, h-16)))
	m.clientTable.SetRows(nil)
	columns := []table.Column{{Title: "Client", Width: listWidth - 2}}
	m.clientTable.SetColumns(columns)
	m.clientTable.SetWidth(listWidth)
	m.setClientRows()
	reservedRows := 39
	if !m.packetDetailsHorizontal {
		reservedRows += 6
	}
	if !m.queueStorageHorizontal {
		reservedRows += 9
	}
	m.packetTable.SetHeight(max(4, h-reservedRows))
	packetWidth := max(24, storageContentWidth)
	if m.packetDetailsHorizontal {
		packetWidth = min(84, storageContentWidth-52)
		m.packetDetailsWidth = storageContentWidth - packetWidth - 4
	}
	m.packetTable.SetWidth(packetWidth)
	m.packetTable.SetRows(nil)
	if packetWidth >= 76 {
		m.packetTable.SetColumns([]table.Column{
			{Title: "Time · Age", Width: 23}, {Title: "Seq", Width: 8},
			{Title: "CAN ID", Width: 10}, {Title: "DLC", Width: 5},
			{Title: "Data", Width: min(23, packetWidth-52)},
		})
	} else {
		m.packetTable.SetColumns([]table.Column{
			{Title: "CAN ID", Width: 10}, {Title: "DLC", Width: 5},
			{Title: "Data", Width: max(8, packetWidth-21)},
		})
	}
	m.setStorageRows()
	m.resizeClientLayout()
	m.drawClientChart()
}

func row(name, data string) string {
	style := value
	if data == "0" || data == "0 B" || data == "0s" || data == "Never" {
		style = muted
	}
	return label.Render(name) + metricColumnGap + style.Render(data)
}

func styledRow(name, data string, style lipgloss.Style) string {
	return label.Render(name) + metricColumnGap + style.Render(data)
}

func metricStyle(active bool, foreground color.Color) lipgloss.Style {
	if !active {
		return muted
	}
	return lipgloss.NewStyle().Foreground(foreground)
}

func formatBytes(bytes uint64) string {
	const unit = 1024
	if bytes < unit {
		return fmt.Sprintf("%d B", bytes)
	}
	div, exp := uint64(unit), 0
	for n := bytes / unit; n >= unit && exp < 3; n /= unit {
		div *= unit
		exp++
	}
	return fmt.Sprintf("%.1f %ciB", float64(bytes)/float64(div), "KMGT"[exp])
}

func formatSignedBytes(bytes int64) string {
	if bytes < 0 {
		return "-" + formatBytes(uint64(-bytes))
	}
	return formatBytes(uint64(bytes))
}

func trend(current, previous float64) string {
	if previous == 0 {
		if current == 0 {
			return "→"
		}
		return "↑"
	}
	change := (current - previous) / previous
	switch {
	case change > 0.05:
		return "↑"
	case change < -0.05:
		return "↓"
	default:
		return "→"
	}
}

func rateText(rate, previous float64, unit string) string {
	return fmt.Sprintf("%s %.1f %s", trend(rate, previous), rate, unit)
}

func utilizationColor(utilization float64) color.Color {
	switch {
	case utilization >= 0.85:
		return red
	case utilization >= 0.60:
		return yellow
	default:
		return green
	}
}

func lagColor(lag uint64) color.Color {
	switch {
	case lag >= 1000:
		return red
	case lag > 0:
		return yellow
	default:
		return green
	}
}

func packetAge(timestamp uint64, now time.Time) time.Duration {
	if timestamp == 0 {
		return 0
	}
	age := now.Sub(time.UnixMilli(int64(timestamp)))
	if age < 0 {
		return 0
	}
	return age
}

func formatAge(age time.Duration) string {
	switch {
	case age < time.Second:
		return fmt.Sprintf("%dms", age.Milliseconds())
	case age < time.Minute:
		return fmt.Sprintf("%.1fs", age.Seconds())
	default:
		return formatDuration(age)
	}
}

func ageColor(age time.Duration) color.Color {
	switch {
	case age >= 10*time.Second:
		return red
	case age >= 2*time.Second:
		return yellow
	default:
		return blue
	}
}

var canPalette = []color.Color{
	lipgloss.Color("#A8D5BA"), lipgloss.Color("#9EC5E6"),
	lipgloss.Color("#D6D3CC"), lipgloss.Color("#E6B8A8"),
	lipgloss.Color("#E6D39A"), lipgloss.Color("#A9D6D2"),
	lipgloss.Color("#D5B8A8"), lipgloss.Color("#B8C9E6"),
}

func canIDStyle(id uint32) lipgloss.Style {
	mixed := id*2654435761 ^ id>>16
	return lipgloss.NewStyle().Foreground(canPalette[int(mixed)%len(canPalette)]).Bold(true)
}

func formatDuration(duration time.Duration) string {
	duration = duration.Round(time.Second)
	hours := duration / time.Hour
	minutes := duration % time.Hour / time.Minute
	seconds := duration % time.Minute / time.Second
	if hours > 0 {
		return fmt.Sprintf("%dh %02dm %02ds", hours, minutes, seconds)
	}
	return fmt.Sprintf("%dm %02ds", minutes, seconds)
}

func (m model) ingressText() string {
	lastReceive := "Never"
	if m.data.Ingress.LastReceiveMS > 0 {
		lastReceive = time.UnixMilli(m.data.Ingress.LastReceiveMS).Local().Format("15:04:05")
	}
	return lipgloss.JoinVertical(lipgloss.Left,
		title.Render("Ingress"),
		styledRow("Active", fmt.Sprint(m.data.Ingress.Active),
			metricStyle(m.data.Ingress.Active > 0, green)),
		row("Total", fmt.Sprint(m.data.Ingress.Total)),
		styledRow("Received", formatBytes(m.data.Ingress.Bytes),
			metricStyle(m.data.Ingress.Bytes > 0, text)),
		row("Reads", fmt.Sprint(m.data.Ingress.Reads)),
		row("Last Receive", lastReceive),
		styledRow("Failures", fmt.Sprintf("%d read  %d accept",
			m.data.Ingress.ReadFailures, m.data.Ingress.AcceptFailures),
			metricStyle(m.data.Ingress.ReadFailures+m.data.Ingress.AcceptFailures > 0, red)),
		styledRow("Rate", fmt.Sprintf("%s  %s",
			rateText(m.byteRate/1024, m.prevByteRate/1024, "KiB/s"),
			rateText(m.readRate, m.prevReadRate, "read/s")),
			metricStyle(m.byteRate > 0 || m.readRate > 0, blue)),
	)
}

func (m *model) resizeChart() {
	available := blockContentWidth(max(24, m.width)) - lipgloss.Width(m.ingressText()) - 2
	m.ingressHorizontal = available >= 28
	if !m.ingressHorizontal {
		available = blockContentWidth(max(24, m.width))
	}
	m.chart.Resize(max(8, available), 9)
}

func (m model) queueSummary(chartHeight int) string {
	utilization := 0.0
	if m.data.Queue.Capacity > 0 {
		utilization = float64(m.data.Queue.Depth) / float64(m.data.Queue.Capacity)
	}
	headroom := max(0, m.data.Queue.Capacity-m.data.Queue.Depth)
	peakUtilization := 0.0
	if m.data.Queue.Capacity > 0 {
		peakUtilization = float64(m.data.Queue.PeakDepth) / float64(m.data.Queue.Capacity)
	}
	queueTrend := "→"
	if m.queueGrowth > 0 {
		queueTrend = "↑"
	} else if m.queueGrowth < 0 {
		queueTrend = "↓"
	}
	depthColor := utilizationColor(utilization)
	if m.queueGrowth > 0 && utilization < 0.85 {
		depthColor = yellow
	}
	summary := lipgloss.JoinVertical(lipgloss.Left,
		title.Render("Queue"),
		styledRow("Depth", fmt.Sprintf("%s %d / %d", queueTrend, m.data.Queue.Depth, m.data.Queue.Capacity),
			metricStyle(m.data.Queue.Depth > 0, depthColor)),
		row("Headroom", fmt.Sprint(headroom)),
		row("Peak", fmt.Sprintf("%d  %.1f%%", m.data.Queue.PeakDepth, peakUtilization*100)),
		styledRow("Blocked", (time.Duration(m.data.Queue.BlockedMS)*time.Millisecond).Round(time.Millisecond).String(),
			metricStyle(m.data.Queue.BlockedMS > 0, yellow)),
		styledRow("Utilization", fmt.Sprintf("%.1f%%", utilization*100),
			metricStyle(m.data.Queue.Depth > 0, utilizationColor(utilization))),
		m.queueProgress.View(),
	)
	if chartHeight < 5 || m.queueRateLen == 0 {
		return summary
	}
	m.drawQueueChart(chartHeight)
	return lipgloss.JoinVertical(lipgloss.Left,
		summary,
		"",
		title.Render("Utilization · 2m"),
		m.queueChart.View(),
	)
}

func (m *model) drawQueueChart(height int) {
	width := max(12, m.queueProgress.Width())
	m.queueChart.Resize(width, max(5, height))
	m.queueChart.ClearAllData()
	if m.queueRateLen == 0 {
		return
	}
	end := m.queueRates[(m.queueRateAt-1+len(m.queueRates))%len(m.queueRates)].Time
	start := end.Add(-2 * time.Minute)
	m.queueChart.SetTimeRange(start, end)
	m.queueChart.SetYRange(0, 100)
	m.queueChart.SetViewTimeAndYRange(start, end, 0, 100)
	for i := 0; i < m.queueRateLen; i++ {
		m.queueChart.Push(m.queueRates[(m.queueRateAt-m.queueRateLen+i+len(m.queueRates))%len(m.queueRates)])
	}
	m.queueChart.DrawBraille()
}

func (m *model) resizeQueueBar() {
	width := 24
	if !m.queueStorageHorizontal {
		width = min(32, blockContentWidth(max(24, m.width)))
	}
	m.queueProgress.SetWidth(max(8, width))
}

func (m *model) setClientRows() {
	rows := make([]table.Row, 0, len(m.data.Clients.Clients))
	for _, client := range m.data.Clients.Clients {
		rows = append(rows, table.Row{client.ID})
	}
	m.clientTable.SetRows(rows)
}

func (m *model) setFocus() {
	m.clientTable.Blur()
	m.packetTable.Blur()
	m.clientTable.SetStyles(tableStyles(false))
	m.packetTable.SetStyles(tableStyles(false))
	switch m.focus {
	case 0:
		m.clientTable.Focus()
		m.clientTable.SetStyles(tableStyles(true))
	case 1:
		m.packetTable.Focus()
		m.packetTable.SetStyles(tableStyles(true))
	}
}

func decodeStorage(storage *storage) {
	storage.Packets = storage.Packets[:0]
	var group []storedChunk
	flush := func() {
		decodeChunkGroup(storage, group)
		group = group[:0]
	}
	var previous uint64
	for i := len(storage.Chunks) - 1; i >= 0; i-- {
		chunk := storage.Chunks[i]
		if len(group) > 0 && (chunk.ID != previous+1 ||
			group[len(group)-1].Captured < group[len(group)-1].Bytes) {
			flush()
		}
		group = append(group, chunk)
		previous = chunk.ID
		if chunk.Captured < chunk.Bytes {
			flush()
		}
	}
	flush()
}

func decodeChunkGroup(storage *storage, chunks []storedChunk) {
	const (
		canpHeaderBytes   = 20
		canpV2PacketBytes = 21
		canpV3PacketBytes = 29
	)

	if len(chunks) == 0 {
		return
	}
	var stream []byte
	for _, chunk := range chunks {
		stream = append(stream, chunk.Data...)
	}

	for offset := 0; offset+canpHeaderBytes <= len(stream); {
		if binary.BigEndian.Uint32(stream[offset:offset+4]) != 0x43414E31 {
			offset++
			continue
		}
		version := binary.BigEndian.Uint16(stream[offset+4 : offset+6])
		packetBytes := 0
		switch version {
		case 2:
			packetBytes = canpV2PacketBytes
		case 3:
			packetBytes = canpV3PacketBytes
		default:
			offset++
			continue
		}
		count := int(binary.BigEndian.Uint16(stream[offset+6 : offset+8]))
		size := canpHeaderBytes + count*packetBytes
		if count == 0 || count > 64 {
			offset++
			continue
		}
		if offset+size > len(stream) {
			break
		}

		seq := binary.BigEndian.Uint32(stream[offset+8 : offset+12])
		timestamp := binary.BigEndian.Uint64(stream[offset+12 : offset+20])
		for i := 0; i < count; i++ {
			packet := stream[offset+canpHeaderBytes+i*packetBytes:]
			dlc := min(packet[4], uint8(8))
			canID := binary.BigEndian.Uint32(packet[0:4])
			data := hex.EncodeToString(packet[5 : 5+dlc])
			var deltas [8]uint16
			for j := range deltas {
				if version == 2 {
					deltas[j] = uint16(packet[13+j])
				} else {
					start := 13 + j*2
					deltas[j] = binary.BigEndian.Uint16(packet[start : start+2])
				}
			}
			storage.Packets = append(storage.Packets, storedPacket{
				Key:       fmt.Sprintf("%d/%d/%d/%d/%s", timestamp, seq, i, canID, data),
				Timestamp: timestamp,
				Seq:       seq,
				Index:     uint16(i),
				CANID:     canID,
				DLC:       dlc,
				Data:      data,
				Deltas:    deltas,
			})
		}
		offset += size
	}
}

func (m *model) mergePackets(packets []storedPacket) int {
	const packetHistoryLimit = 4096

	added := 0
	for _, packet := range packets {
		if _, exists := m.packetSeen[packet.Key]; exists {
			continue
		}
		m.packetSeen[packet.Key] = struct{}{}
		m.packetHistory = append(m.packetHistory, packet)
		added++
	}
	sort.Slice(m.packetHistory, func(i, j int) bool {
		a, b := m.packetHistory[i], m.packetHistory[j]
		if a.Timestamp != b.Timestamp {
			return a.Timestamp < b.Timestamp
		}
		if a.Seq != b.Seq {
			return a.Seq < b.Seq
		}
		return a.Index < b.Index
	})
	if len(m.packetHistory) <= packetHistoryLimit {
		return added
	}
	drop := len(m.packetHistory) - packetHistoryLimit
	for _, packet := range m.packetHistory[:drop] {
		delete(m.packetSeen, packet.Key)
	}
	copy(m.packetHistory, m.packetHistory[drop:])
	m.packetHistory = m.packetHistory[:packetHistoryLimit]
	return added
}

func tableStyles(focused bool) table.Styles {
	styles := table.DefaultStyles()
	styles.Cell = styles.Cell.UnsetForeground()
	styles.Header = styles.Header.Foreground(dim).Bold(true)
	styles.Selected = styles.Selected.Foreground(white).
		Background(lipgloss.Color("#333333")).Bold(true)
	if focused {
		styles.Header = styles.Header.Foreground(white)
		styles.Selected = styles.Selected.Foreground(lipgloss.Color("#000000")).
			Background(white).Bold(true)
	}
	return styles
}

func focusView(name, body string, focused bool) string {
	marker := "  "
	style := muted
	if focused {
		marker = "▶ "
		style = title
	}
	return lipgloss.JoinVertical(lipgloss.Left, style.Render(marker+name), body)
}

func (m model) controls() string {
	if m.filtering {
		return title.Render("Filter CAN ID  ") + m.filterInput.View() +
			muted.Render("  Enter apply  Esc clear")
	}
	context := []string{"Clients", "Packets"}[m.focus]
	filter := "all"
	if m.filterActive {
		filter = fmt.Sprintf("0x%03X", m.filterCANID)
	}
	if m.width < 36 {
		return muted.Render("Tab  /  s  ↑↓  q")
	}
	if m.width < 55 {
		return muted.Render("Tab focus  / filter  s sort  ↑/↓  q")
	}
	if m.width < 110 {
		return muted.Render(fmt.Sprintf(
			"%s · %s  / filter  s sort  ↑/↓ select  Tab focus  q quit",
			context, filter,
		))
	}
	return muted.Render(fmt.Sprintf(
		"%s  filter %s  / filter  s sort  ↑/↓ select  PgUp/PgDn page  Tab focus  Ctrl+↑/↓ dashboard  q quit",
		context, filter,
	))
}

func (m *model) setStorageRows() {
	rows := make([]table.Row, 0, len(m.packetHistory))
	keys := make([]string, 0, len(m.packetHistory))
	full := m.packetTable.Width() >= 76
	followLatest := m.packetTable.Cursor() == 0
	selected := m.selectedPacketKey
	selectedIndex := -1
	packets := append([]storedPacket(nil), m.packetHistory...)
	sort.SliceStable(packets, func(i, j int) bool {
		a, b := packets[i], packets[j]
		switch m.packetSort {
		case sortCANIDAscending:
			if a.CANID != b.CANID {
				return a.CANID < b.CANID
			}
		case sortCANIDDescending:
			if a.CANID != b.CANID {
				return a.CANID > b.CANID
			}
		}
		if a.Timestamp != b.Timestamp {
			return a.Timestamp > b.Timestamp
		}
		if a.Seq != b.Seq {
			return a.Seq > b.Seq
		}
		return a.Index > b.Index
	})
	now := m.data.CurrentTime
	if now.IsZero() {
		now = time.Now()
	}
	for _, packet := range packets {
		if m.filterActive && packet.CANID != m.filterCANID {
			continue
		}
		if packet.Key == selected {
			selectedIndex = len(rows)
		}
		keys = append(keys, packet.Key)
		canID := canIDStyle(packet.CANID).Render(fmt.Sprintf("0x%03X", packet.CANID))
		if full {
			age := packetAge(packet.Timestamp, now)
			timeAndAge := time.UnixMilli(int64(packet.Timestamp)).Local().Format("15:04:05.000") +
				" " + lipgloss.NewStyle().Foreground(ageColor(age)).Render(formatAge(age))
			rows = append(rows, table.Row{
				timeAndAge,
				fmt.Sprint(packet.Seq),
				canID, fmt.Sprint(packet.DLC), spacedHex(packet.Data),
			})
		} else {
			rows = append(rows, table.Row{
				canID, fmt.Sprint(packet.DLC), spacedHex(packet.Data),
			})
		}
	}
	m.packetKeys = keys
	m.packetTable.SetRows(rows)
	if followLatest && len(rows) > 0 {
		m.packetTable.SetCursor(0)
		m.selectedPacketKey = keys[0]
	} else if selectedIndex >= 0 {
		m.packetTable.SetCursor(selectedIndex)
	} else if len(rows) > 0 {
		m.packetTable.SetCursor(0)
		m.selectedPacketKey = keys[0]
	}
}

func (m *model) applyFilter() {
	text := strings.TrimSpace(m.filterInput.Value())
	if text == "" {
		m.filterActive = false
		return
	}
	text = strings.TrimPrefix(strings.ToLower(text), "0x")
	id, err := strconv.ParseUint(text, 16, 32)
	if err != nil {
		m.filterActive = false
		return
	}
	m.filterCANID = uint32(id)
	m.filterActive = true
}

func (m *model) rememberSelectedPacket() {
	cursor := m.packetTable.Cursor()
	if cursor >= 0 && cursor < len(m.packetKeys) {
		m.selectedPacketKey = m.packetKeys[cursor]
	}
}

func spacedHex(data string) string {
	bytes, err := hex.DecodeString(data)
	if err != nil {
		return data
	}
	parts := make([]string, len(bytes))
	for i, b := range bytes {
		parts[i] = fmt.Sprintf("%02X", b)
	}
	return strings.Join(parts, " ")
}

func (m model) selectedPacket() (storedPacket, bool) {
	key := m.selectedPacketKey
	for _, packet := range m.packetHistory {
		if packet.Key == key {
			return packet, true
		}
	}
	return storedPacket{}, false
}

func (m model) packetSortName() string {
	switch m.packetSort {
	case sortCANIDAscending:
		return "CAN ↑"
	case sortCANIDDescending:
		return "CAN ↓"
	default:
		return "newest"
	}
}

func (m model) packetStats() string {
	ids := make(map[uint32]struct{})
	count := 0
	for _, packet := range m.packetHistory {
		if m.filterActive && packet.CANID != m.filterCANID {
			continue
		}
		ids[packet.CANID] = struct{}{}
		count++
	}
	rate := metricStyle(m.packetRate > 0, blue).Render(
		rateText(m.packetRate, m.prevPacketRate, "pkt/s"))
	counts := value.Render(fmt.Sprintf("%d shown  ·  %d CAN IDs", count, len(ids)))
	sortMode := muted.Render("sort " + m.packetSortName())
	if m.packetTable.Width() < 72 {
		return lipgloss.JoinVertical(lipgloss.Left,
			title.Render("Packets"),
			lipgloss.JoinHorizontal(lipgloss.Left, rate, "  ", counts),
			sortMode,
		)
	}
	return lipgloss.JoinHorizontal(lipgloss.Left,
		title.Render("Packets"), "    ", rate, "    ", counts, "    ", sortMode)
}

func (m model) packetDetails() string {
	packet, ok := m.selectedPacket()
	if !ok {
		return muted.Render("Select a packet to inspect it")
	}
	now := m.data.CurrentTime
	if now.IsZero() {
		now = time.Now()
	}
	age := packetAge(packet.Timestamp, now)
	data, _ := hex.DecodeString(packet.Data)
	dataParts := make([]string, len(data))
	deltaParts := make([]string, len(data))
	for i, b := range data {
		dataParts[i] = fmt.Sprintf("%-4s", fmt.Sprintf("%02X", b))
		deltaParts[i] = fmt.Sprintf("%-4d", packet.Deltas[i])
	}
	batch := time.UnixMilli(int64(packet.Timestamp)).Local().Format("Jan 02 15:04:05.000")
	batchAndAge := batch + "  ·  " +
		lipgloss.NewStyle().Foreground(ageColor(age)).Render(formatAge(age)+" old")
	return lipgloss.JoinVertical(lipgloss.Left,
		title.Render("Selected Packet"),
		styledRow("CAN ID", fmt.Sprintf("0x%03X", packet.CANID), canIDStyle(packet.CANID)),
		row("Data", strings.TrimRight(strings.Join(dataParts, ""), " ")),
		row("Δt", strings.TrimRight(strings.Join(deltaParts, ""), " ")),
		row("Batch", batchAndAge),
	)
}

func (m model) clientSummary() string {
	lastSend := "Never"
	if m.data.Clients.LastSendMS > 0 {
		lastSend = time.UnixMilli(m.data.Clients.LastSendMS).Local().Format("15:04:05")
	}
	return lipgloss.JoinVertical(lipgloss.Left,
		title.Render("Clients"),
		styledRow("Active", fmt.Sprint(m.data.Clients.Active),
			metricStyle(m.data.Clients.Active > 0, green)),
		row("Total", fmt.Sprint(m.data.Clients.Total)),
		styledRow("Sent", formatBytes(m.data.Clients.Bytes),
			metricStyle(m.data.Clients.Bytes > 0, text)),
		row("Messages", fmt.Sprint(m.data.Clients.Messages)),
		styledRow("Max Lag", fmt.Sprint(m.data.Clients.MaxLag),
			metricStyle(m.data.Clients.MaxLag > 0, lagColor(m.data.Clients.MaxLag))),
		row("Last Send", lastSend),
		styledRow("Failures", fmt.Sprintf("%d write  %d accept",
			m.data.Clients.WriteFailures, m.data.Clients.AcceptFailures),
			metricStyle(m.data.Clients.WriteFailures+m.data.Clients.AcceptFailures > 0, red)),
		styledRow("Rate", fmt.Sprintf("%s  %s",
			rateText(m.clientByteRate/1024, m.prevClientByteRate/1024, "KiB/s"),
			rateText(m.clientMessageRate, m.prevClientMessageRate, "msg/s")),
			metricStyle(m.clientByteRate > 0 || m.clientMessageRate > 0, blue)),
	)
}

func (m model) clientDetails() string {
	client, ok := m.selectedClient()
	if !ok {
		return title.Render("No Client Selected")
	}
	rate := m.clientRates[client.ID]
	if rate == nil {
		rate = &clientRate{}
	}
	last := "Never"
	if client.LastSendMS > 0 {
		last = time.UnixMilli(client.LastSendMS).Local().Format("15:04:05")
	}
	return lipgloss.JoinVertical(lipgloss.Left,
		title.Render(client.ID),
		row("Uptime", (time.Duration(m.data.CurrentTime.UnixMilli()-client.ConnectedMS)*
			time.Millisecond).Round(time.Second).String()),
		styledRow("Lag", fmt.Sprint(client.Lag),
			metricStyle(client.Lag > 0, lagColor(client.Lag))),
		styledRow("Sent", formatBytes(client.Bytes), metricStyle(client.Bytes > 0, text)),
		row("Msgs", fmt.Sprint(client.Messages)),
		styledRow("Rate", rateText(rate.byteRate/1024, rate.prevByteRate/1024, "KiB/s"),
			metricStyle(rate.byteRate > 0, blue)),
		row("Last", last),
		styledRow("Fail", fmt.Sprint(client.WriteFailures),
			metricStyle(client.WriteFailures > 0, red)),
	)
}

func (m *model) resizeClientLayout() {
	inner := blockContentWidth(max(24, m.width))
	summaryWidth := lipgloss.Width(m.clientSummary())
	detailWidth := lipgloss.Width(m.clientDetails())
	listWidth := m.clientTable.Width()

	chartWidth := inner - summaryWidth - listWidth - detailWidth - 6
	m.clientsHorizontal = chartWidth >= 28
	m.clientsTwoRows = false
	if !m.clientsHorizontal {
		chartWidth = inner - detailWidth - 2
		m.clientsTwoRows = chartWidth >= 28 && inner >= summaryWidth+listWidth+2
	}
	if !m.clientsHorizontal && !m.clientsTwoRows {
		chartWidth = inner
	}
	m.clientChart.Resize(max(8, chartWidth), max(9, m.clientTable.Height()+1))
}

func (m *model) drawRateChart() {
	if m.rateLen == 0 {
		return
	}
	var high float64
	for i := 0; i < m.rateLen; i++ {
		if rate := m.rates[(m.rateAt-m.rateLen+i+len(m.rates))%len(m.rates)].Value; rate > high {
			high = rate
		}
	}
	if high == 0 {
		high = 1
	}
	end := m.rates[(m.rateAt-1+len(m.rates))%len(m.rates)].Time
	start := end.Add(-time.Minute)
	m.chart.ClearAllData()
	m.chart.SetTimeRange(start, end)
	m.chart.SetYRange(0, high*1.1)
	m.chart.SetViewTimeAndYRange(start, end, 0, high*1.1)
	for i := 0; i < m.rateLen; i++ {
		m.chart.Push(m.rates[(m.rateAt-m.rateLen+i+len(m.rates))%len(m.rates)])
	}
	m.chart.DrawBraille()
}

func (m model) selectedClient() (client, bool) {
	selected := m.clientTable.SelectedRow()
	if len(selected) == 0 {
		return client{}, false
	}
	for _, client := range m.data.Clients.Clients {
		if client.ID == selected[0] {
			return client, true
		}
	}
	return client{}, false
}

func (m *model) drawClientChart() {
	client, ok := m.selectedClient()
	if !ok {
		m.clientChart.ClearAllData()
		return
	}
	rate := m.clientRates[client.ID]
	if rate == nil || rate.len == 0 {
		m.clientChart.ClearAllData()
		return
	}
	var high float64
	for i := 0; i < rate.len; i++ {
		if point := rate.points[(rate.at-rate.len+i+len(rate.points))%len(rate.points)]; point.Value > high {
			high = point.Value
		}
	}
	if high == 0 {
		high = 1
	}
	end := rate.points[(rate.at-1+len(rate.points))%len(rate.points)].Time
	start := end.Add(-time.Minute)
	m.clientChart.ClearAllData()
	m.clientChart.SetTimeRange(start, end)
	m.clientChart.SetYRange(0, high*1.1)
	m.clientChart.SetViewTimeAndYRange(start, end, 0, high*1.1)
	for i := 0; i < rate.len; i++ {
		m.clientChart.Push(rate.points[(rate.at-rate.len+i+len(rate.points))%len(rate.points)])
	}
	m.clientChart.DrawBraille()
}

func (m model) storageHeader() string {
	return lipgloss.JoinVertical(lipgloss.Left,
		title.Render("Storage"),
		styledRow("Disk", formatSignedBytes(m.data.Storage.DiskBytes),
			metricStyle(m.data.Storage.DiskBytes > 0, text)),
	)
}

func (m model) View() tea.View {
	outerWidth := max(24, m.width)
	contentWidth := blockContentWidth(outerWidth)
	content := ""
	statusStyle := lipgloss.NewStyle().Foreground(yellow).Bold(true)
	content = statusStyle.Render("Connecting...")
	if m.err != nil {
		statusStyle = lipgloss.NewStyle().Foreground(red).Bold(true)
		content = statusStyle.Render("Server unavailable") + "\n\n" + muted.Render(m.err.Error())
	} else if !m.data.StartTime.IsZero() {
		dot := " "
		if m.dot {
			dot = "●"
		}
		statusStyle = lipgloss.NewStyle().Foreground(green).Bold(true)
		content = statusStyle.Render(dot+"  Server Connected") + "  " +
			value.Render(formatDuration(time.Duration(m.data.UptimeMS)*time.Millisecond))
	}

	right := value.Render(m.data.CurrentTime.Local().Format("Jan 02 15:04:05 MST"))
	processRows := []string{
		row("PID", fmt.Sprint(m.data.PID)),
		row("Goroutines", fmt.Sprint(m.data.Goroutines)),
		styledRow("Memory", formatBytes(m.data.MemoryBytes),
			metricStyle(m.data.MemoryBytes > 0, text)),
	}
	processText := lipgloss.JoinVertical(lipgloss.Left, processRows...)
	if m.width >= 85 {
		processText = lipgloss.JoinHorizontal(lipgloss.Top,
			processRows[0], "  ", processRows[1], "  ", processRows[2])
	}
	headerBody := lipgloss.JoinVertical(lipgloss.Left, content, processText, right)
	headerUsed := lipgloss.Width(content) + lipgloss.Width(processText) + lipgloss.Width(right)
	if contentWidth >= headerUsed+4 {
		flexibleGap := contentWidth - headerUsed - 2
		headerBody = lipgloss.JoinHorizontal(lipgloss.Top,
			content, "  ", processText,
			lipgloss.NewStyle().Width(flexibleGap).Render(""),
			right)
	}
	header := renderBlock(outerWidth, headerBody)

	ingressText := m.ingressText()
	ingressBody := lipgloss.JoinVertical(lipgloss.Left, ingressText, m.chart.View())
	if m.ingressHorizontal {
		ingressBody = lipgloss.JoinHorizontal(lipgloss.Top, ingressText, "  ", m.chart.View())
	}
	ingress := renderBlock(outerWidth, ingressBody)
	clientSummary := m.clientSummary()
	details := m.clientDetails()
	sections := []string{
		clientSummary,
		m.clientTable.View(),
		details,
		m.clientChart.View(),
	}
	clientsBody := lipgloss.JoinVertical(lipgloss.Left, sections...)
	if m.clientsHorizontal {
		clientsBody = lipgloss.JoinHorizontal(lipgloss.Top,
			sections[0], "  ", sections[1], "  ", sections[2], "  ", sections[3])
	} else if m.clientsTwoRows {
		clientsBody = lipgloss.JoinVertical(lipgloss.Left,
			lipgloss.JoinHorizontal(lipgloss.Top, sections[0], "  ", sections[1]),
			lipgloss.JoinHorizontal(lipgloss.Top, sections[2], "  ", sections[3]),
		)
	}
	clients := renderBlock(outerWidth, clientsBody)
	packetList := lipgloss.JoinVertical(lipgloss.Left,
		m.packetStats(),
		m.packetTable.View(),
	)
	packetArea := lipgloss.JoinVertical(lipgloss.Left,
		packetList,
		"",
		m.packetDetails(),
	)
	if m.packetDetailsHorizontal {
		details := lipgloss.NewStyle().
			Width(m.packetDetailsWidth).
			MaxWidth(m.packetDetailsWidth).
			Render(m.packetDetails())
		packetArea = lipgloss.JoinHorizontal(lipgloss.Top,
			packetList,
			lipgloss.NewStyle().Width(4).Render(""),
			details,
		)
	}
	storageBody := lipgloss.JoinVertical(lipgloss.Left,
		m.storageHeader(),
		"",
		packetArea,
	)
	queueStorage := ""
	fixedHeight := lipgloss.Height(header) + lipgloss.Height(ingress) + lipgloss.Height(clients)
	if m.queueStorageHorizontal {
		const queueOuterWidth = 34
		storageOuterWidth := outerWidth - queueOuterWidth - 1
		storageNatural := renderBlock(storageOuterWidth, storageBody)
		bottomHeight := max(
			lipgloss.Height(storageNatural),
			m.page.Height()-fixedHeight,
		)
		queueBase := m.queueSummary(0)
		queueChartHeight := max(5,
			bottomHeight-panel.GetVerticalFrameSize()-lipgloss.Height(queueBase)-3)
		queueBody := m.queueSummary(queueChartHeight)
		queueStorage = lipgloss.JoinHorizontal(lipgloss.Top,
			renderBlockHeight(queueOuterWidth, bottomHeight, queueBody),
			" ",
			renderBlockHeight(storageOuterWidth, bottomHeight, storageBody),
		)
	} else {
		queueBody := m.queueSummary(0)
		queue := renderBlock(outerWidth, queueBody)
		storageNatural := renderBlock(outerWidth, storageBody)
		storageHeight := max(
			lipgloss.Height(storageNatural),
			m.page.Height()-fixedHeight-lipgloss.Height(queue),
		)
		queueStorage = lipgloss.JoinVertical(lipgloss.Left,
			queue,
			renderBlockHeight(outerWidth, storageHeight, storageBody),
		)
	}

	body := lipgloss.JoinVertical(lipgloss.Left,
		header, ingress, clients, queueStorage)
	page := m.page
	page.SetContent(body)
	view := tea.NewView(lipgloss.JoinVertical(lipgloss.Left, page.View(), m.controls()))
	view.AltScreen = true
	return view
}

func newModel(decoder *json.Decoder) model {
	chart := tslc.New(40, 9,
		tslc.WithXYSteps(2, 2),
		tslc.WithXLabelFormatter(tslc.HourTimeLabelFormatter()),
		tslc.WithStyle(lipgloss.NewStyle().Foreground(blue)),
		tslc.WithAxesStyles(muted, muted),
	)
	clientChart := tslc.New(40, 10,
		tslc.WithXYSteps(2, 2),
		tslc.WithXLabelFormatter(tslc.HourTimeLabelFormatter()),
		tslc.WithStyle(lipgloss.NewStyle().Foreground(blue)),
		tslc.WithAxesStyles(muted, muted),
	)
	queueChart := tslc.New(24, 8,
		tslc.WithXYSteps(2, 2),
		tslc.WithXLabelFormatter(tslc.HourTimeLabelFormatter()),
		tslc.WithStyle(lipgloss.NewStyle().Foreground(yellow)),
		tslc.WithAxesStyles(muted, muted),
	)
	queueProgress := progress.New(
		progress.WithColors(white),
		progress.WithWidth(24),
		progress.WithSpringOptions(18, 1),
		progress.WithoutPercentage(),
	)
	queueProgress.EmptyColor = dim
	clientTable := table.New(
		table.WithColumns([]table.Column{
			{Title: "Client", Width: 26},
		}),
		table.WithHeight(10),
		table.WithFocused(true),
		table.WithStyles(tableStyles(true)),
	)
	packetTable := table.New(
		table.WithColumns([]table.Column{
			{Title: "Time · Age", Width: 23}, {Title: "Seq", Width: 8},
			{Title: "CAN ID", Width: 10}, {Title: "DLC", Width: 5},
			{Title: "Data", Width: 23},
		}),
		table.WithHeight(10),
		table.WithStyles(tableStyles(false)),
	)
	filterInput := textinput.New()
	filterInput.Prompt = "/"
	filterInput.Placeholder = "0x3FF"
	filterInput.CharLimit = 10
	filterInput.SetWidth(12)
	m := model{
		decoder:       decoder,
		chart:         chart,
		clientChart:   clientChart,
		queueChart:    queueChart,
		queueProgress: queueProgress,
		clientRates:   make(map[string]*clientRate),
		packetSeen:    make(map[string]struct{}),
		clientTable:   clientTable,
		packetTable:   packetTable,
		filterInput:   filterInput,
		page:          viewport.New(viewport.WithWidth(80), viewport.WithHeight(24)),
		width:         80,
		height:        24,
	}
	m.resize()
	m.setFocus()
	return m
}

func main() {
	conn, err := net.Dial("tcp", "3.141.38.115:5400")
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	defer conn.Close()
	m := newModel(json.NewDecoder(conn))
	if _, err := tea.NewProgram(m).Run(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
