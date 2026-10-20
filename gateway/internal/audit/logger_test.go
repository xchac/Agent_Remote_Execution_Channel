package audit

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestParamsHashIsCanonical(t *testing.T) {
	left, err := ParamsHash(map[string]any{"path": "/tmp/a", "max_bytes": 10})
	if err != nil {
		t.Fatal(err)
	}
	right, err := ParamsHash(map[string]any{"max_bytes": 10, "path": "/tmp/a"})
	if err != nil {
		t.Fatal(err)
	}
	if left != right {
		t.Fatal("parameter order must not change the hash")
	}
}

func TestLoggerWritesRedactedRecord(t *testing.T) {
	path := filepath.Join(t.TempDir(), "audit.jsonl")
	logger, err := New(path)
	if err != nil {
		t.Fatal(err)
	}
	if err := logger.Write(Record{
		SessionID:  "session-1",
		RequestID:  "request-1",
		Tool:       "system_info",
		ParamsHash: strings.Repeat("a", 64),
		Status:     "ok",
	}); err != nil {
		t.Fatal(err)
	}
	if err := logger.Close(); err != nil {
		t.Fatal(err)
	}
	body, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(body), `"request_id":"request-1"`) {
		t.Fatalf("unexpected audit record: %s", body)
	}
	if strings.Contains(string(body), "token") || strings.Contains(string(body), "secret") {
		t.Fatalf("audit record leaked sensitive fields: %s", body)
	}
}
