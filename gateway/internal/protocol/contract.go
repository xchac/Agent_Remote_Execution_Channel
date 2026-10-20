package protocol

const (
	Version        = "1"
	MaxFrameBytes  = 4 * 1024 * 1024
	TokenVersion   = 1
	DefaultTTL     = 900
	DefaultCalls   = 100
	DefaultTimeout = 5000
	MaxTimeout     = 30000
	Retention      = 10 * 60 * 1000
	MaxCacheItems  = 1024
)

const (
	StatusPending = "pending"
	StatusRunning = "running"
	StatusOK      = "ok"
	StatusTimeout = "timeout"
	StatusFailed  = "failed"
	StatusUnknown = "unknown"
)

const (
	ErrToolNotFound        = "TOOL_NOT_FOUND"
	ErrUnauthorized        = "UNAUTHORIZED"
	ErrRateLimited         = "RATE_LIMITED"
	ErrInvalidParams       = "INVALID_PARAMS"
	ErrExecutionTimeout    = "EXECUTION_TIMEOUT"
	ErrExecutionFailed     = "EXECUTION_FAILED"
	ErrOutputTooLarge      = "OUTPUT_TOO_LARGE"
	ErrProtocol            = "PROTOCOL_ERROR"
	ErrIdempotencyConflict = "IDEMPOTENCY_CONFLICT"
)

type ToolDefinition struct {
	Name         string         `json:"name"`
	Description  string         `json:"description"`
	Platform     []string       `json:"platform"`
	Idempotent   bool           `json:"idempotent"`
	ParamsSchema map[string]any `json:"params_schema"`
}

var SupportedTools = []ToolDefinition{
	{
		Name:        "system_info",
		Description: "Return hostname, operating system, and architecture.",
		Platform:    []string{"darwin", "windows"},
		Idempotent:  true,
		ParamsSchema: map[string]any{
			"type":                 "object",
			"properties":           map[string]any{},
			"additionalProperties": false,
		},
	},
	{
		Name:        "exec_command",
		Description: "Execute a shell command and return stdout, stderr, and exit code.",
		Platform:    []string{"darwin", "windows"},
		Idempotent:  false,
		ParamsSchema: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"command": map[string]any{"type": "string", "minLength": 1},
				"cwd":     map[string]any{"type": "string"},
			},
			"required":             []string{"command"},
			"additionalProperties": false,
		},
	},
	{
		Name:        "read_file",
		Description: "Read a file and return its content.",
		Platform:    []string{"darwin", "windows"},
		Idempotent:  true,
		ParamsSchema: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"path":      map[string]any{"type": "string", "minLength": 1},
				"max_bytes": map[string]any{"type": "integer", "minimum": 1, "maximum": 262144},
			},
			"required":             []string{"path"},
			"additionalProperties": false,
		},
	},
}

func ToolByName(name string) (ToolDefinition, bool) {
	for _, tool := range SupportedTools {
		if tool.Name == name {
			return tool, true
		}
	}
	return ToolDefinition{}, false
}

func SupportedToolNames() []string {
	names := make([]string, 0, len(SupportedTools))
	for _, tool := range SupportedTools {
		names = append(names, tool.Name)
	}
	return names
}
