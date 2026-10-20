package state

import (
	"testing"
	"time"

	"arec/gateway/internal/protocol"
)

func record(id string) Record {
	return Record{
		RequestID:      id,
		SessionID:      "session",
		Tool:           "system_info",
		ParamsHash:     "hash",
		IdempotencyKey: "key",
		Target:         "auto",
		Status:         protocol.StatusPending,
	}
}

func TestBeginIdempotencyAndConflict(t *testing.T) {
	store := New(time.Minute, 10)
	first := store.Begin(record("request-1"))
	if !first.Created {
		t.Fatal("first request should be created")
	}
	duplicate := store.Begin(record("request-2"))
	if duplicate.Created || duplicate.Conflict || duplicate.Record.RequestID != "request-1" {
		t.Fatalf("unexpected duplicate result: %+v", duplicate)
	}
	conflictRecord := record("request-3")
	conflictRecord.ParamsHash = "different"
	conflict := store.Begin(conflictRecord)
	if !conflict.Conflict {
		t.Fatalf("expected idempotency conflict, got %+v", conflict)
	}
}

func TestStoreCapacityEviction(t *testing.T) {
	store := New(time.Minute, 2)
	store.Begin(record("request-1"))
	second := record("request-2")
	second.IdempotencyKey = "key-2"
	store.Begin(second)
	third := record("request-3")
	third.IdempotencyKey = "key-3"
	store.Begin(third)

	if _, ok := store.Get("request-1"); ok {
		t.Fatal("oldest record should be evicted")
	}
	if _, ok := store.Get("request-3"); !ok {
		t.Fatal("newest record should remain")
	}
}

func TestStoreExpiry(t *testing.T) {
	store := New(10*time.Millisecond, 10)
	store.Begin(record("request-1"))
	time.Sleep(20 * time.Millisecond)
	if _, ok := store.Get("request-1"); ok {
		t.Fatal("expired record should be removed")
	}
}
