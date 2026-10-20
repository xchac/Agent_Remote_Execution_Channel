package session

import (
	"errors"
	"testing"
	"time"

	"golang.org/x/time/rate"
)

func newTestManager() *Manager {
	return NewManager(Config{
		Secret:       []byte("test-secret"),
		AllowedTools: []string{"system_info", "exec_command", "read_file"},
		MaxTTL:       15 * time.Minute,
		MaxCalls:     100,
		SessionRate:  rate.Inf,
		SessionBurst: 100,
		ToolRate:     rate.Inf,
		ToolBurst:    100,
	})
}

func TestCreateAuthorizeAndConsume(t *testing.T) {
	manager := newTestManager()
	created, err := manager.Create([]string{"system_info"}, 0, 1)
	if err != nil {
		t.Fatal(err)
	}
	info, err := manager.Authorize(created.Token, "system_info")
	if err != nil {
		t.Fatal(err)
	}
	if err := manager.Consume(info, "system_info"); err != nil {
		t.Fatal(err)
	}
	if err := manager.Consume(info, "system_info"); !errors.Is(err, ErrUnauthorized) {
		t.Fatalf("expected call limit rejection, got %v", err)
	}
	if _, err := manager.Authorize(created.Token, "exec_command"); !errors.Is(err, ErrUnauthorized) {
		t.Fatalf("expected tool scope rejection, got %v", err)
	}
}

func TestCannotExpandSessionLimits(t *testing.T) {
	manager := newTestManager()
	if _, err := manager.Create([]string{"system_info"}, 901, 1); !errors.Is(err, ErrUnauthorized) {
		t.Fatalf("expected ttl rejection, got %v", err)
	}
	if _, err := manager.Create([]string{"system_info"}, 900, 101); !errors.Is(err, ErrUnauthorized) {
		t.Fatalf("expected max-calls rejection, got %v", err)
	}
}

func TestRateLimit(t *testing.T) {
	manager := NewManager(Config{
		Secret:       []byte("test-secret"),
		AllowedTools: []string{"system_info"},
		MaxTTL:       time.Minute,
		MaxCalls:     10,
		SessionRate:  1,
		SessionBurst: 1,
		ToolRate:     rate.Inf,
		ToolBurst:    10,
	})
	created, err := manager.Create([]string{"system_info"}, 60, 10)
	if err != nil {
		t.Fatal(err)
	}
	info, err := manager.Authorize(created.Token, "system_info")
	if err != nil {
		t.Fatal(err)
	}
	if err := manager.Consume(info, "system_info"); err != nil {
		t.Fatal(err)
	}
	if err := manager.Consume(info, "system_info"); !errors.Is(err, ErrRateLimited) {
		t.Fatalf("expected rate limit, got %v", err)
	}
}
