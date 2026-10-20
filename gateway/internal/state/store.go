package state

import (
	"encoding/json"
	"sync"
	"time"

	"arec/gateway/internal/protocol"
)

type Record struct {
	RequestID      string          `json:"request_id"`
	SessionID      string          `json:"session_id"`
	TokenID        string          `json:"token_id,omitempty"`
	Tool           string          `json:"tool"`
	ParamsHash     string          `json:"params_hash"`
	IdempotencyKey string          `json:"-"`
	Target         string          `json:"target"`
	Platform       string          `json:"platform,omitempty"`
	AgentID        string          `json:"-"`
	Status         string          `json:"status"`
	Result         json.RawMessage `json:"result,omitempty"`
	ErrorCode      *string         `json:"error_code"`
	Retryable      bool            `json:"retryable"`
	Message        string          `json:"message,omitempty"`
	ExecutedAt     *string         `json:"executed_at,omitempty"`
	CreatedAt      time.Time       `json:"-"`
	UpdatedAt      time.Time       `json:"-"`
}

type BeginResult struct {
	Record   Record
	Created  bool
	Conflict bool
}

type Store struct {
	mu        sync.Mutex
	retention time.Duration
	maxItems  int
	records   map[string]Record
	order     []string
	idem      map[string]string
}

func New(retention time.Duration, maxItems int) *Store {
	if retention <= 0 {
		retention = time.Duration(protocol.Retention) * time.Millisecond
	}
	if maxItems <= 0 {
		maxItems = protocol.MaxCacheItems
	}
	return &Store{
		retention: retention,
		maxItems:  maxItems,
		records:   make(map[string]Record),
		idem:      make(map[string]string),
	}
}

func (s *Store) Begin(record Record) BeginResult {
	s.mu.Lock()
	defer s.mu.Unlock()

	now := time.Now().UTC()
	s.cleanupLocked(now)
	key := idemKey(record.SessionID, record.IdempotencyKey)
	if requestID, ok := s.idem[key]; ok {
		existing, exists := s.records[requestID]
		if !exists {
			delete(s.idem, key)
		} else if existing.Tool != record.Tool || existing.ParamsHash != record.ParamsHash {
			return BeginResult{Record: clone(existing), Conflict: true}
		} else {
			return BeginResult{Record: clone(existing)}
		}
	}

	record.CreatedAt = now
	record.UpdatedAt = now
	if record.Status == "" {
		record.Status = protocol.StatusPending
	}
	s.records[record.RequestID] = record
	s.order = append(s.order, record.RequestID)
	if record.IdempotencyKey != "" {
		s.idem[key] = record.RequestID
	}
	s.trimLocked()
	return BeginResult{Record: clone(record), Created: true}
}

func (s *Store) Get(requestID string) (Record, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.cleanupLocked(time.Now().UTC())
	record, ok := s.records[requestID]
	if !ok {
		return Record{}, false
	}
	return clone(record), true
}

func (s *Store) Update(requestID string, update func(*Record)) (Record, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	record, ok := s.records[requestID]
	if !ok {
		return Record{}, false
	}
	update(&record)
	record.UpdatedAt = time.Now().UTC()
	s.records[requestID] = record
	return clone(record), true
}

func (s *Store) Delete(requestID string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	record, ok := s.records[requestID]
	if !ok {
		return
	}
	delete(s.records, requestID)
	delete(s.idem, idemKey(record.SessionID, record.IdempotencyKey))
	s.removeOrderLocked(requestID)
}

func IsFinal(status string) bool {
	return status == protocol.StatusOK ||
		status == protocol.StatusTimeout ||
		status == protocol.StatusFailed
}

func (s *Store) cleanupLocked(now time.Time) {
	cutoff := now.Add(-s.retention)
	for requestID, record := range s.records {
		if record.UpdatedAt.Before(cutoff) {
			delete(s.records, requestID)
			delete(s.idem, idemKey(record.SessionID, record.IdempotencyKey))
			s.removeOrderLocked(requestID)
		}
	}
}

func (s *Store) trimLocked() {
	for len(s.records) > s.maxItems && len(s.order) > 0 {
		requestID := s.order[0]
		s.order = s.order[1:]
		record, ok := s.records[requestID]
		if !ok {
			continue
		}
		delete(s.records, requestID)
		delete(s.idem, idemKey(record.SessionID, record.IdempotencyKey))
	}
}

func (s *Store) removeOrderLocked(requestID string) {
	for index, value := range s.order {
		if value == requestID {
			s.order = append(s.order[:index], s.order[index+1:]...)
			return
		}
	}
}

func idemKey(sessionID, key string) string {
	return sessionID + "\x00" + key
}

func clone(record Record) Record {
	result := record
	if record.Result != nil {
		result.Result = append(json.RawMessage(nil), record.Result...)
	}
	if record.ErrorCode != nil {
		value := *record.ErrorCode
		result.ErrorCode = &value
	}
	if record.ExecutedAt != nil {
		value := *record.ExecutedAt
		result.ExecutedAt = &value
	}
	return result
}
