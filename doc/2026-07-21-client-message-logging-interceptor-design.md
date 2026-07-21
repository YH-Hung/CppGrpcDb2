# Client-side message logging interceptor — design

Date: 2026-07-21
Status: approved

## Goal

Mirror the existing server-side `MessageLoggingServerInterceptor` on the client:
log each outgoing request and incoming response of `greeter_failover_client` as
JSON via spdlog, without the `grpc_client_lb` library learning about
interceptors.

## Component

New files `include/message_logging_client_interceptor.h` and
`src/interceptor/message_logging_client_interceptor.cpp`, added to the existing
`message_logging_interceptor` CMake target (organized by concern; no new
library).

- `MessageLoggingClientInterceptor` (a `grpc::experimental::Interceptor`),
  constructed with the RPC method name:
  - `PRE_SEND_MESSAGE`: cast `GetSendMessage()` to
    `google::protobuf::Message`, convert with `MessageToJsonString`
    (`preserve_proto_field_names = true`), log
    `[<method>] Request message (JSON): {...}` at info level. Conversion
    failure logs a warn; the interceptor never throws and always calls
    `Proceed()`.
  - `POST_RECV_MESSAGE`: same for `GetRecvMessage()`, logged as
    `Response message (JSON)`.
- `MessageLoggingClientInterceptorFactory` (a
  `grpc::experimental::ClientInterceptorFactoryInterface`) creates one
  interceptor per RPC, capturing `ClientRpcInfo::method()`.
- No otel trace-id in the log line: the failover client sets up no tracer, so
  it would print all-zeros noise. Method name only. (The server variant keeps
  its trace id.)

## Integration

`greeter_failover_client.cpp` passes a custom `ChannelBuilder` to
`FailoverClient<Greeter>::FromEnv(stub_factory, builder)`. The builder calls
`grpc::experimental::CreateCustomChannelWithInterceptors(endpoint.Target(),
InsecureChannelCredentials(), args, creators)` — same target, credentials, and
`args` (from `MakeChannelArguments`) as the default builder, just with the
interceptor factory attached. The binary links `message_logging_interceptor`.

Interceptor logs go through spdlog, which the demo already routes to stderr, so
stdout stays stable for the live test script.

## Error handling

Identical posture to the server interceptor: JSON conversion failures and
unexpected exceptions are logged at warn and swallowed; the RPC always
proceeds. A null message pointer is skipped silently.

## Testing

- GTest `tests/interceptor/test_message_logging_client_interceptor.cpp`
  (wired via `create_test_executable`): starts an in-process Greeter server on
  `127.0.0.1:0` (same pattern as `tests/lb/test_failover_live.cpp`), creates a
  channel with `CreateCustomChannelWithInterceptors` and the factory, issues
  `SayHello`, asserts the RPC succeeds and that both the request and response
  JSON lines were logged (captured via a spdlog test sink).
- Manual: run `greeter_failover_client` against a local greeter server and
  observe the two JSON log lines per call on stderr.

## Rejected alternatives

- Baking interceptor support into `channel_factory` /
  `ChannelFactoryOptions`: keeps coupling out of the lb library; the
  `ChannelBuilder` injection point already exists for exactly this kind of
  customization.
- A separate CMake library for the client interceptor: an extra target with no
  benefit; the existing `message_logging_interceptor` target already carries
  the concern.
- Logging per-attempt `grpc::Status`: rejected to keep exact parity with the
  server interceptor; status is already visible in the demo's stdout.
