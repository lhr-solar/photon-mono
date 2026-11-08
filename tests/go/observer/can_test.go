package main

import (
	"bytes"
	"encoding/json"
	"strings"
	"testing"
	"time"

	tea "charm.land/bubbletea/v2"
)

func connected(t *testing.T, commands *bytes.Buffer) model {
	t.Helper()
	m := newModel(json.NewDecoder(strings.NewReader("")), commands)
	var data metadata
	data.StartTime = time.Now()
	data.CurrentTime = time.Now()
	data.CAN.Frames = 10
	data.Config.CAN.DBCDir = "dbc/spec/dbc"
	next, _ := m.Update(data)
	return next.(model)
}

func press(t *testing.T, m model, key tea.KeyPressMsg) (model, tea.Cmd) {
	t.Helper()
	next, cmd := m.Update(key)
	return next.(model), cmd
}

func run(cmd tea.Cmd) {
	if cmd != nil {
		cmd()
	}
}

func sent(t *testing.T, commands *bytes.Buffer) map[string]any {
	t.Helper()
	var line struct {
		Set struct {
			CAN map[string]any `json:"can"`
		} `json:"set"`
	}
	if err := json.Unmarshal(commands.Bytes(), &line); err != nil {
		t.Fatalf("command %q: %v", commands.String(), err)
	}
	commands.Reset()
	return line.Set.CAN
}

func TestToggleDecodingSendsCommand(t *testing.T) {
	var commands bytes.Buffer
	m := connected(t, &commands)
	m, cmd := press(t, m, tea.KeyPressMsg{Code: 'e', Text: "e"})
	run(cmd)
	if got := sent(t, &commands); got["decode"] != true || len(got) != 1 {
		t.Errorf("sent %v, want decode=true", got)
	}
	if m.canStatus == "" {
		t.Errorf("no status shown while waiting for the server")
	}
}

func TestEditDBCDirSendsCommand(t *testing.T) {
	var commands bytes.Buffer
	m := connected(t, &commands)
	m, _ = press(t, m, tea.KeyPressMsg{Code: 'c', Text: "c"})
	if !m.editingDBC || m.dbcInput.Value() != "dbc/spec/dbc" {
		t.Fatalf("editing=%v value=%q, want the current dir to edit", m.editingDBC, m.dbcInput.Value())
	}
	m.dbcInput.SetValue("other/dbc")
	m, cmd := press(t, m, tea.KeyPressMsg{Code: tea.KeyEnter})
	run(cmd)
	if got := sent(t, &commands); got["dbc_dir"] != "other/dbc" {
		t.Errorf("sent %v, want dbc_dir=other/dbc", got)
	}
	if m.editingDBC {
		t.Errorf("still editing after Enter")
	}

	// Esc leaves without sending anything.
	m, _ = press(t, m, tea.KeyPressMsg{Code: 'c', Text: "c"})
	_, cmd = press(t, m, tea.KeyPressMsg{Code: tea.KeyEscape})
	run(cmd)
	if commands.Len() != 0 {
		t.Errorf("Esc sent %q", commands.String())
	}
}

// A reply line has no metadata; it must not wipe what the dashboard shows.
func TestReplyKeepsMetadata(t *testing.T) {
	var commands bytes.Buffer
	m := connected(t, &commands)
	next, _ := m.Update(metadata{Reply: &reply{Error: "read DBC dir: no such file"}})
	m = next.(model)
	if m.data.StartTime.IsZero() || m.data.CAN.Frames != 10 {
		t.Errorf("reply replaced the metadata: %+v", m.data.CAN)
	}
	if m.canStatusOK || !strings.Contains(m.canStatus, "no such file") {
		t.Errorf("status = %q ok=%v, want the server's error", m.canStatus, m.canStatusOK)
	}
	next, _ = m.Update(metadata{Reply: &reply{OK: true}})
	if m = next.(model); !m.canStatusOK || m.canStatus != "applied" {
		t.Errorf("status = %q ok=%v, want applied", m.canStatus, m.canStatusOK)
	}
}

// Before the first metadata line there is nothing to toggle or edit.
func TestNoCommandsBeforeConnected(t *testing.T) {
	var commands bytes.Buffer
	m := newModel(json.NewDecoder(strings.NewReader("")), &commands)
	m, cmd := press(t, m, tea.KeyPressMsg{Code: 'e', Text: "e"})
	run(cmd)
	m, _ = press(t, m, tea.KeyPressMsg{Code: 'c', Text: "c"})
	if commands.Len() != 0 || m.editingDBC {
		t.Errorf("sent %q editing=%v before connecting", commands.String(), m.editingDBC)
	}
}

func TestCANPanelShowsSettings(t *testing.T) {
	var commands bytes.Buffer
	m := connected(t, &commands)
	text := m.canText()
	for _, want := range []string{"CAN", "off", "dbc/spec/dbc", "Sample rates"} {
		if !strings.Contains(text, want) {
			t.Errorf("CAN panel is missing %q:\n%s", want, text)
		}
	}
}
