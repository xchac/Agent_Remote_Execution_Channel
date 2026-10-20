package protocol

import "testing"

func TestSupportedTools(t *testing.T) {
	if len(SupportedTools) != 3 {
		t.Fatalf("expected 3 tools, got %d", len(SupportedTools))
	}
	for _, name := range SupportedToolNames() {
		if _, ok := ToolByName(name); !ok {
			t.Fatalf("tool %q not found", name)
		}
	}
}
