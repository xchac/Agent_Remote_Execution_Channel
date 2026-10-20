package main

import (
	"context"
	"errors"
	"fmt"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"arec/gateway/internal/gateway"
)

func main() {
	if err := run(); err != nil {
		log.Printf("fatal: %v", err)
		os.Exit(1)
	}
}

func run() error {
	config := gateway.Config{
		BootstrapKey: os.Getenv("AREC_BOOTSTRAP_KEY"),
		HMACSecret:   os.Getenv("AREC_HMAC_SECRET"),
		AuditPath:    envOr("AREC_AUDIT_PATH", "var/audit/arec-audit.jsonl"),
	}
	httpAddress := envOr("AREC_HTTP_ADDR", ":8080")
	dataAddress := envOr("AREC_DATA_ADDR", ":9000")

	app, err := gateway.New(config)
	if err != nil {
		return err
	}
	defer app.Close()
	if err := app.StartData(dataAddress); err != nil {
		return fmt.Errorf("start data plane: %w", err)
	}

	server := &http.Server{
		Addr:              httpAddress,
		Handler:           app.Handler(),
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       35 * time.Second,
		WriteTimeout:      35 * time.Second,
	}
	serverErrors := make(chan error, 1)
	go func() {
		log.Printf("HTTP listening on %s, data plane listening on %s", httpAddress, app.DataAddr())
		serverErrors <- server.ListenAndServe()
	}()

	stop := make(chan os.Signal, 1)
	signal.Notify(stop, syscall.SIGINT, syscall.SIGTERM)
	select {
	case received := <-stop:
		log.Printf("received %s; shutting down", received)
	case err := <-serverErrors:
		if !errors.Is(err, http.ErrServerClosed) {
			return err
		}
	}

	shutdownContext, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	return server.Shutdown(shutdownContext)
}

func envOr(name, fallback string) string {
	if value := os.Getenv(name); value != "" {
		return value
	}
	return fallback
}
