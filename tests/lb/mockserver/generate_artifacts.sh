#!/usr/bin/env bash
# Generates the runtime artifacts the MockServer compose stack needs, all
# under gen/ (gitignored — never check these in):
#   gen/helloworld.dsc            proto descriptor set (request decoding)
#   gen/expectations/mockN.json   per-instance SayHello expectation
#
# Why the expectations are generated instead of static: MockServer 7.4.0
# decodes gRPC *requests* via the descriptor, but does NOT re-encode JSON
# response bodies to protobuf (its docs claim otherwise), and it only emits
# gRPC trailers when the expectation defines them explicitly. A JSON body
# therefore reaches gRPC clients as raw text ("Bad GRPC frame type 0x7b").
# The workaround: each expectation carries the reply pre-encoded as gRPC
# wire bytes — a 5-byte frame header (compressed-flag 0x00 + 4-byte
# big-endian length) followed by the serialized helloworld.HelloReply — as
# a BINARY base64 body, plus a content-type: application/grpc header and an
# explicit grpc-status: 0 trailer.
#
# The reply text "Hello from mockN @5005N" is a test interface:
# run_live_test.sh asserts on it verbatim.
set -eu -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"

command -v protoc >/dev/null 2>&1 || { echo "protoc not found on PATH" >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "python3 not found on PATH" >&2; exit 2; }

mkdir -p "${SCRIPT_DIR}/gen/expectations"

protoc --descriptor_set_out="${SCRIPT_DIR}/gen/helloworld.dsc" \
       --include_imports -I "${REPO_ROOT}/protos" \
       "${REPO_ROOT}/protos/helloworld.proto"

frame_reply_b64() {  # frame_reply_b64 <reply message text>
    printf 'message: "%s"' "$1" \
      | protoc --encode=helloworld.HelloReply -I "${REPO_ROOT}/protos" \
               "${REPO_ROOT}/protos/helloworld.proto" \
      | python3 -c 'import sys, base64, struct
pb = sys.stdin.buffer.read()
sys.stdout.write(base64.b64encode(b"\x00" + struct.pack(">I", len(pb)) + pb).decode())'
}

for N in 1 2 3; do
    B64="$(frame_reply_b64 "Hello from mock${N} @5005${N}")"
    cat > "${SCRIPT_DIR}/gen/expectations/mock${N}.json" <<EOF
[
  {
    "httpRequest": {
      "method": "POST",
      "path": "/helloworld.Greeter/SayHello"
    },
    "httpResponse": {
      "statusCode": 200,
      "headers": { "content-type": ["application/grpc"] },
      "body": { "type": "BINARY", "base64Bytes": "${B64}" },
      "trailers": { "grpc-status": ["0"] }
    }
  }
]
EOF
done

echo "generated: gen/helloworld.dsc + gen/expectations/mock{1,2,3}.json"
