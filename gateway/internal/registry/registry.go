package registry

import (
	"sort"
	"sync"
	"time"

	"arec/gateway/internal/protocol"
)

type Endpoint struct {
	AgentID      string
	ConnectionID string
	Platform     string
	Tools        []protocol.ToolDefinition
	ConnectedAt  time.Time
}

func (e Endpoint) Supports(tool string) bool {
	for _, definition := range e.Tools {
		if definition.Name == tool {
			return true
		}
	}
	return false
}

type Registry struct {
	mu    sync.RWMutex
	items map[string]Endpoint
}

func New() *Registry {
	return &Registry{items: make(map[string]Endpoint)}
}

func (r *Registry) Register(endpoint Endpoint) (Endpoint, bool) {
	r.mu.Lock()
	defer r.mu.Unlock()
	previous, existed := r.items[endpoint.AgentID]
	r.items[endpoint.AgentID] = endpoint
	return previous, existed
}

func (r *Registry) Unregister(agentID, connectionID string) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	endpoint, ok := r.items[agentID]
	if !ok || endpoint.ConnectionID != connectionID {
		return false
	}
	delete(r.items, agentID)
	return true
}

func (r *Registry) Endpoint(agentID string) (Endpoint, bool) {
	r.mu.RLock()
	defer r.mu.RUnlock()
	endpoint, ok := r.items[agentID]
	return endpoint, ok
}

func (r *Registry) Candidates(tool, target, preferredPlatform string) []Endpoint {
	r.mu.RLock()
	defer r.mu.RUnlock()

	candidates := make([]Endpoint, 0)
	for _, endpoint := range r.items {
		if !endpoint.Supports(tool) {
			continue
		}
		if target != "auto" && endpoint.Platform != target {
			continue
		}
		candidates = append(candidates, endpoint)
	}
	sort.Slice(candidates, func(i, j int) bool {
		left, right := candidates[i], candidates[j]
		if target == "auto" {
			leftPreferred := left.Platform == preferredPlatform
			rightPreferred := right.Platform == preferredPlatform
			if leftPreferred != rightPreferred {
				return leftPreferred
			}
			if left.Platform != right.Platform {
				if left.Platform == "darwin" {
					return true
				}
				if right.Platform == "darwin" {
					return false
				}
			}
		}
		return left.AgentID < right.AgentID
	})
	return candidates
}

func (r *Registry) Snapshot() []Endpoint {
	r.mu.RLock()
	defer r.mu.RUnlock()
	result := make([]Endpoint, 0, len(r.items))
	for _, endpoint := range r.items {
		result = append(result, endpoint)
	}
	sort.Slice(result, func(i, j int) bool {
		return result[i].AgentID < result[j].AgentID
	})
	return result
}
