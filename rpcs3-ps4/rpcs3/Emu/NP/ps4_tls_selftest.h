#pragma once

// Boot TLS self-test of the console's HTTPS stack (libcurl + wolfSSL), without internet. The implementation
// (ps4_tls_selftest.cpp) is built in the NP target, which has exceptions enabled and the wolfSSL ABI defines; this
// header includes neither wolfSSL nor curl, so frontend files built without exceptions can call it. No exception
// leaves these functions.
//
// Steps, all on a "TLS Self-test" thread: curl_version_info; curl_multi_init() (the call curl_easy_perform makes
// first); wolfSSL RNG (32 bytes from the libSceRandom-seeded DRBG); the package CA bundle /app0/certs/cacert.pem
// loaded the way curl loads CURLOPT_CAINFO_BLOB (root count and time); one HTTPS request over loopback to a
// wolfSSL server on 127.0.0.1 (wolfSSL test certificate, CN/SAN example.com), made with the options of the
// patch download plus CURLOPT_RESOLVE; the test CA is trusted on that one curl handle only.
// Result in startup.log, e.g.
// "TLS self-test: libcurl 8.22.0-DEV wolfSSL/5.9.4; multi ok; RNG ok; CA bundle 148 roots in 31 ms; loopback
//  https://example.com:49152 -> 127.0.0.1: curl 0, HTTP 200 'ok', TLSv1.3 TLS13-AES128-GCM-SHA256, 84 ms
//  -> TLS OK (no internet used)"

#include <optional>
#include <string>

// Starts the self-test once (later calls do nothing) and returns at once. Calls curl_global_init(CURL_GLOBAL_DEFAULT)
// on the calling thread first, so call it from the main thread, after the native socket ABI probe
// (verify_native_socket_address): curl's non-blocking sockets use the SO_NBIO route that probe proves.
void ps4_tls_selftest_start() noexcept;

// std::nullopt while the test runs, before it was started, or when it could not run (a thread refused by the
// console; the line says which). true: every step passed. false: a step failed; the line names it.
std::optional<bool> ps4_tls_selftest_ok() noexcept;

// The startup.log line of the finished test ("TLS self-test: ..."); "" while it runs or before it was started.
std::string ps4_tls_selftest_line() noexcept;
