# OpenTelemetry Observability Stack

This directory contains the Docker Compose setup for local development and testing of OpenTelemetry tracing.

## Stack Components

The `grafana/otel-lgtm` container provides an all-in-one observability stack:

- **Grafana** (port 3000): Visualization and dashboards
- **Tempo**: Distributed tracing backend
- **Loki**: Log aggregation
- **Prometheus/Mimir**: Metrics storage
- **OpenTelemetry Collector**: Receives and processes telemetry data

## Quick Start

### Start the observability stack:
```bash
cd docker
docker compose up -d
```

### Check status:
```bash
docker compose ps
docker compose logs -f otel-lgtm
```

### Access Grafana:
1. Open http://localhost:3000 in your browser
2. Default credentials: `admin` / `admin` (you'll be prompted to change)
3. Navigate to **Explore** → **Tempo** to view traces

### Stop the stack:
```bash
docker compose down
```

### Clean up everything (including volumes):
```bash
docker compose down -v
```

## Configuration

- **otel-collector-config.yaml**: OpenTelemetry Collector configuration
  - Receives OTLP gRPC on port 4317
  - Receives OTLP HTTP on port 4318
  - Exports traces to Tempo
  - Includes logging exporter for debugging

## Application Configuration

Configure your C++ application to send traces to the collector:

```bash
export OTEL_EXPORTER_OTLP_ENDPOINT=http://localhost:4317
export OTEL_SERVICE_NAME=my-grpc-service
```

Or in code (see `src/tracing/tracer_provider.cpp`):
```cpp
auto exporter = opentelemetry::exporter::otlp::OtlpGrpcExporterFactory::Create(
    opentelemetry::exporter::otlp::OtlpGrpcExporterOptions{
        .endpoint = "localhost:4317",
        .use_ssl_credentials = false
    }
);
```

## Troubleshooting

### Collector not receiving traces
```bash
# Check collector logs
docker compose logs otel-lgtm | grep -i otlp

# Verify ports are accessible
curl -v http://localhost:4318/v1/traces
nc -zv localhost 4317
```

### Grafana not showing traces
1. Wait 10-30 seconds for traces to appear
2. Check time range in Grafana (top right)
3. Verify Tempo data source is configured (should be automatic)
4. Check collector logs for export errors

### Container health check failing
```bash
# Check container health
docker compose ps

# View detailed logs
docker compose logs otel-lgtm

# Restart the stack
docker compose restart
```

## Ports Reference

| Port | Service | Description |
|------|---------|-------------|
| 3000 | Grafana | Web UI |
| 4317 | OTLP | gRPC receiver |
| 4318 | OTLP | HTTP receiver |
| 9090 | Prometheus | Metrics (optional) |
| 8888 | Collector | Internal metrics |
| 8889 | Collector | Prometheus exporter |

## Next Steps

1. Build and run your C++ gRPC application
2. Make some gRPC calls to generate traces
3. View traces in Grafana at http://localhost:3000
4. Explore trace propagation across multiple services
5. Correlate logs with traces using trace IDs

## Advanced Configuration

To customize the collector configuration, edit `otel-collector-config.yaml` and restart:
```bash
docker compose restart otel-lgtm
```

For production deployments, consider:
- Using persistent volumes for data retention
- Configuring authentication and TLS
- Separating collector from storage backends
- Implementing sampling strategies
- Setting up alerts and SLOs
