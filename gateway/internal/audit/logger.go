package audit

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"os"
	"path/filepath"
	"sync"
	"time"
)

type Record struct {
	Timestamp      string  `json:"timestamp"`
	SessionID      string  `json:"session_id,omitempty"`
	TokenID        string  `json:"token_id,omitempty"`
	RequestID      string  `json:"request_id,omitempty"`
	Tool           string  `json:"tool,omitempty"`
	ParamsHash     string  `json:"params_hash,omitempty"`
	TargetPlatform string  `json:"target_platform,omitempty"`
	Status         string  `json:"status"`
	ErrorCode      *string `json:"error_code"`
	DurationMS     int64   `json:"duration_ms"`
	ExecutedAt     *string `json:"executed_at,omitempty"`
	Event          string  `json:"event,omitempty"`
}

type Logger struct {
	mu   sync.Mutex
	file *os.File
}

func New(path string) (*Logger, error) {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return nil, err
	}
	file, err := os.OpenFile(path, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o600)
	if err != nil {
		return nil, err
	}
	return &Logger{file: file}, nil
}

func (l *Logger) Write(record Record) error {
	if record.Timestamp == "" {
		record.Timestamp = time.Now().UTC().Format(time.RFC3339)
	}
	line, err := json.Marshal(record)
	if err != nil {
		return err
	}
	line = append(line, '\n')

	l.mu.Lock()
	defer l.mu.Unlock()
	_, err = l.file.Write(line)
	return err
}

func (l *Logger) Close() error {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.file.Close()
}

func ParamsHash(value any) (string, error) {
	canonical, err := json.Marshal(value)
	if err != nil {
		return "", err
	}
	sum := sha256.Sum256(canonical)
	return hex.EncodeToString(sum[:]), nil
}
