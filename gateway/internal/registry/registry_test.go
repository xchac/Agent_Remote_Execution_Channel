package registry

import (
	"testing"
	"time"

	"arec/gateway/internal/protocol"
)

func tool(name string) protocol.ToolDefinition {
	definition, _ := protocol.ToolByName(name)
	return definition
}

func TestCandidatesAndAutoOrdering(t *testing.T) {
	r := New()
	r.Register(Endpoint{AgentID: "win", ConnectionID: "1", Platform: "windows", Tools: []protocol.ToolDefinition{tool("system_info")}, ConnectedAt: time.Now()})
	r.Register(Endpoint{AgentID: "mac", ConnectionID: "2", Platform: "darwin", Tools: []protocol.ToolDefinition{tool("system_info"), tool("read_file")}, ConnectedAt: time.Now()})

	if got := r.Candidates("read_file", "windows", ""); len(got) != 0 {
		t.Fatalf("windows should not support read_file without registration, got %+v", got)
	}
	if got := r.Candidates("system_info", "auto", ""); len(got) != 2 || got[0].Platform != "darwin" {
		t.Fatalf("expected darwin-first auto route, got %+v", got)
	}
	if got := r.Candidates("system_info", "auto", "windows"); len(got) != 2 || got[0].Platform != "windows" {
		t.Fatalf("expected preferred windows route, got %+v", got)
	}
}

func TestUnregisterChecksConnectionIdentity(t *testing.T) {
	r := New()
	r.Register(Endpoint{AgentID: "mac", ConnectionID: "new", Platform: "darwin"})
	if r.Unregister("mac", "old") {
		t.Fatal("old connection must not remove replacement")
	}
	if !r.Unregister("mac", "new") {
		t.Fatal("current connection should be removed")
	}
}
