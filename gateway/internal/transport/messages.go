package transport

import (
	"encoding/json"

	"arec/gateway/internal/protocol"
)

type Message struct {
	Type            string                    `json:"type"`
	ProtocolVersion string                    `json:"protocol_version"`
	AgentID         string                    `json:"agent_id,omitempty"`
	Platform        string                    `json:"platform,omitempty"`
	Tools           []protocol.ToolDefinition `json:"tools,omitempty"`
	Accepted        *bool                     `json:"accepted,omitempty"`
	RequestID       string                    `json:"request_id,omitempty"`
	SessionID       string                    `json:"session_id,omitempty"`
	Tool            string                    `json:"tool,omitempty"`
	Params          json.RawMessage           `json:"params,omitempty"`
	IdempotencyKey  string                    `json:"idempotency_key,omitempty"`
	DeadlineMS      int                       `json:"deadline_ms,omitempty"`
	Token           string                    `json:"token,omitempty"`
	Status          string                    `json:"status,omitempty"`
	Result          json.RawMessage           `json:"result,omitempty"`
	ErrorCode       *string                   `json:"error_code,omitempty"`
	Retryable       bool                      `json:"retryable,omitempty"`
	Message         string                    `json:"message,omitempty"`
	ExecutedAt      string                    `json:"executed_at,omitempty"`
}
