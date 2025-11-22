# Quickstart: OpenTelemetry Tracing for gRPC Services

**Feature**: 001-otel-grpc-tracing
**Date**: 2025-11-21
**Purpose**: Get started with distributed tracing in under 10 minutes

## Prerequisites

- Docker installed and running
- CppGrpcDb2 project built successfully
- OpenTelemetry C++ SDK installed (via Homebrew or built from source)

## Step 1: Start OTLP Collector and Grafana

**Run the grafana/otel-lgtm all-in-one observability stack:**

```bash
docker run -d \
  --name otel-lgtm \
  -p 3000:3000 \
  -p 4317:4317 \
  -p 4318:4318 \
  grafana/otel-lgtm:latest
```

**Verify container is running:**
```bash
docker ps | grep otel-lgtm
```

**Access Grafana UI:**
- Open browser: http://localhost:3000
- Default credentials: `admin` / `admin`
- (You'll be prompted to change password on first login)

**Ports exposed:**
- **3000**: Grafana web UI
- **4317**: OTLP gRPC receiver (for traces)
- **4318**: OTLP HTTP receiver (alternative endpoint)

## Step 2: Build Project with Tracing Enabled

**From project root directory:**

```bash
# Clean previous build (optional)
rm -rf build

# Configure with CMake
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_PREFIX_PATH=$HOME/.local \
      -DCMAKE_INSTALL_PREFIX=$HOME/.local \
      .. --fresh

# Build all targets
cmake --build . --parallel 4
```

**Verify build succeeded:**
```bash
ls -lh greeter_server greeter_client
```

Expected output: Both binaries should exist and be executable.

## Step 3: Configure Environment Variables

**Set OTLP exporter endpoint:**

```bash
export OTEL_EXPORTER_OTLP_ENDPOINT="http://localhost:4317"
export OTEL_SERVICE_NAME="greeter_server"
export OTEL_RESOURCE_ATTRIBUTES="deployment.environment=dev"
```

**Optional: Enable debug logging (verbose tracing output)**
```bash
export OTEL_LOG_LEVEL=debug
```

## Step 4: Run Instrumented gRPC Server

**Start the server with tracing enabled:**

```bash
# From build directory
./greeter_server
```

**Expected output:**
```
Server listening on 0.0.0.0:50051
OpenTelemetry TracerProvider initialized
OTLP exporter configured: http://localhost:4317
Tracing enabled for service: greeter_server
```

**Keep server running in this terminal.** Open a new terminal for the next step.

## Step 5: Make RPC Calls to Generate Traces

**Open new terminal and run client:**

```bash
# From build directory
export OTEL_SERVICE_NAME="greeter_client"
export OTEL_EXPORTER_OTLP_ENDPOINT="http://localhost:4317"

./greeter_client localhost:50051 "World"
```

**Expected output:**
```
Greeter received: Hello World
Trace ID: 4bf92f3577b34da6a3ce929d0e0e4736
```

**Make multiple requests to generate more traces:**
```bash
for i in {1..10}; do
  ./greeter_client localhost:50051 "Request-$i"
  sleep 1
done
```

## Step 6: View Traces in Grafana

**Navigate to Grafana Explore:**

1. Open browser: http://localhost:3000
2. Click **Explore** (compass icon) in left sidebar
3. Select **Tempo** from datasource dropdown (top-left)
4. In query section:
   - Query type: **Search**
   - Service Name: **greeter_server** or **greeter_client**
   - Click **Run query**

**Expected result:** List of traces with trace IDs, durations, and span counts

**Click on a trace** to view detailed span timeline:
- Server span: `/helloworld.Greeter/SayHello`
- Client span (if client instrumented): Nested under request
- Parent-child relationships visualized

**View trace details:**
- **Trace ID**: Unique identifier for the entire request
- **Span ID**: Unique identifier for each operation
- **Duration**: Time spent in each span
- **Attributes**: RPC method, service name, status code, etc.

## Step 7: Verify Log Correlation

**Check server logs for trace IDs:**

```bash
# Look for logs with trace_id and span_id
grep "trace_id" server.log
```

**Expected log format:**
```
[2025-11-21 10:30:45.123] [info] [trace_id=4bf92f3577b34da6a3ce929d0e0e4736] [span_id=00f067aa0ba902b7] Processing request: World
```

**Copy trace_id from logs** and search in Grafana Explore:
1. Query type: **TraceID**
2. Paste trace ID
3. Click **Run query**

**Expected result:** Full trace visualization matching the trace ID from logs

## Step 8: Test Multi-Service Propagation (Optional)

**If you have multiple services, test trace propagation:**

1. **Start first server** (already running from Step 4)
2. **Start second server** (e.g., order service):
   ```bash
   export OTEL_SERVICE_NAME="order_server"
   ./order_server
   ```
3. **Make client call** that triggers chain:
   ```bash
   ./greeter_client localhost:50051 "ChainTest"
   ```
4. **View in Grafana**: Trace should show spans from both services with correct parent-child relationships

## Troubleshooting

### Traces Not Appearing in Grafana

**Check OTLP collector is reachable:**
```bash
curl -v http://localhost:4317
```
Expected: Connection successful (or gRPC-specific error)

**Check Docker container logs:**
```bash
docker logs otel-lgtm
```
Look for errors in OTLP receiver or Tempo

**Verify exporter endpoint in application:**
```bash
echo $OTEL_EXPORTER_OTLP_ENDPOINT
```
Should be: `http://localhost:4317`

**Check application logs for tracing errors:**
```bash
grep -i "trace\|span\|otlp" server.log
```

### Invalid or Missing Trace Context

**Symptom:** Traces appear as separate root spans instead of connected tree

**Cause:** Trace context not propagating correctly

**Solution:**
- Verify client interceptor injects `traceparent` metadata
- Verify server interceptor extracts `traceparent` metadata
- Check for errors in trace context parsing
- Run integration test: `./test_trace_propagation`

### High Latency or Performance Issues

**Symptom:** Requests are slower with tracing enabled

**Solution:**
- Check batch span processor configuration (should be async)
- Verify OTLP exporter is not blocking request processing
- Monitor memory usage: `ps aux | grep greeter_server`
- Adjust batch size or schedule delay in TracerProvider config
- Consider reducing sampling rate if overhead >5%

### Compilation Errors

**Symptom:** Build fails with OpenTelemetry header not found

**Solution:**
```bash
# Verify OpenTelemetry installation
find $HOME/.local -name "opentelemetry" -type d

# On macOS
brew list opentelemetry-cpp

# Re-configure CMake with correct prefix
cmake -DCMAKE_PREFIX_PATH=$HOME/.local ..
```

## Quick Validation Checklist

- [ ] Docker container `otel-lgtm` running and healthy
- [ ] Grafana accessible at http://localhost:3000
- [ ] Tempo datasource configured in Grafana (should be automatic)
- [ ] Server starts without errors, logs "Tracing enabled"
- [ ] Client makes requests successfully
- [ ] Traces appear in Grafana Explore → Tempo
- [ ] Trace IDs visible in application logs
- [ ] Spans have correct attributes (rpc.method, rpc.service, etc.)
- [ ] Parent-child relationships correct in trace visualization
- [ ] Log trace_id matches trace ID in Grafana

## Next Steps

**Once basic tracing is working:**

1. **Add custom spans** for database queries:
   ```cpp
   auto span = tracer->StartSpan("DB2Query");
   auto scope = tracer->WithActiveSpan(span);
   // Execute query
   span->SetAttribute("db.statement", sql);
   ```

2. **Add span events** for key operations:
   ```cpp
   span->AddEvent("Connection acquired", {{"pool.size", 10}});
   ```

3. **Configure sampling** to reduce overhead:
   ```cpp
   auto sampler = TraceIdRatioBased(0.1);  // 10% sampling
   ```

4. **Integrate with CI/CD**:
   - Run integration tests with OTLP collector
   - Verify trace export in automated tests
   - Set up trace-based alerts (e.g., high error rate)

5. **Production deployment**:
   - Deploy OTLP collector in production environment
   - Configure production OTLP endpoint via environment variables
   - Set up persistent Tempo storage
   - Create Grafana dashboards for trace analytics

## Example Queries in Grafana

**Find slow requests (duration >100ms):**
```
Query type: Search
Service: greeter_server
Min duration: 100ms
```

**Find failed requests:**
```
Query type: Search
Service: greeter_server
Tags: rpc.grpc.status_code != 0
```

**Find requests by RPC method:**
```
Query type: Search
Service: greeter_server
Tags: rpc.method = SayHello
```

## Clean Up

**Stop and remove Docker container:**
```bash
docker stop otel-lgtm
docker rm otel-lgtm
```

**Disable tracing:**
```bash
unset OTEL_EXPORTER_OTLP_ENDPOINT
unset OTEL_SERVICE_NAME
```

**Remove trace data:**
```bash
# Container volumes are ephemeral, data removed on container removal
```

## Additional Resources

- **OpenTelemetry C++ Documentation**: https://opentelemetry.io/docs/languages/cpp/
- **Grafana Tempo Documentation**: https://grafana.com/docs/tempo/latest/
- **W3C Trace Context**: https://www.w3.org/TR/trace-context/
- **gRPC Interceptors**: https://grpc.io/docs/languages/cpp/interceptors/
- **Project Design Doc**: `doc/opentelemetry_tracing.md`
- **Trace Context Contract**: `specs/001-otel-grpc-tracing/contracts/trace-context-propagation.md`

## Support

For issues or questions:
1. Check application logs for errors
2. Check Docker container logs: `docker logs otel-lgtm`
3. Review design doc for architecture details
4. Run integration tests to isolate issue
5. Open GitHub issue with logs and reproduction steps
