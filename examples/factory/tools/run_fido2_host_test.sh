#!/usr/bin/env bash
# Build and run the FIDO2 CTAP2 core host conformance test.
#
# The core is platform-independent; on the host the crypto layer binds to
# OpenSSL (FACTORY_FIDO_HOST_CRYPTO) and the test verifies assertions with an
# independent OpenSSL code path. Run from anywhere.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${TMPDIR:-/tmp}/fido2_host_test"

OPENSSL_PREFIX="$(brew --prefix openssl@3 2>/dev/null || brew --prefix openssl 2>/dev/null || echo /usr/local/opt/openssl@3)"

"${CXX:-c++}" -std=c++17 -O2 -Wall -Wno-deprecated-declarations \
    -DFACTORY_FIDO_HOST_CRYPTO \
    -I"$OPENSSL_PREFIX/include" \
    "$ROOT/fido2_core.cpp" "$ROOT/fido2_cbor.cpp" "$ROOT/fido2_crypto.cpp" \
    "$ROOT/tools/fido2_host_test.cpp" \
    -L"$OPENSSL_PREFIX/lib" -lssl -lcrypto \
    -o "$OUT"

exec "$OUT"
