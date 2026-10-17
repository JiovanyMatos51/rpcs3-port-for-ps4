#include "stdafx.h"
#include "ps4_tls_selftest.h"

#ifdef RPCS3_PS4

// Built in the NP target (exceptions on, wolfSSL ABI defines); the frontend only sees ps4_tls_selftest.h.
// Nothing here leaves the console: the only connection is 127.0.0.1 to a server thread of this file.

#include "Utilities/File.h"
#include "Utilities/Thread.h"

#include <curl/curl.h>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wold-style-cast"
#pragma clang diagnostic ignored "-Wextern-c-compat"
#pragma clang diagnostic ignored "-Wunused-const-variable"
#pragma clang diagnostic ignored "-Wunused-variable"
#endif
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/random.h>
// wolfSSL's own test server certificate (CN www.wolfssl.com, SAN DNS:example.com and IP:127.0.0.1) and its CA, as
// DER arrays; identical to 3rdparty/wolfssl/wolfssl/certs/{server-cert,server-key,ca-cert}.der.
#ifndef USE_CERT_BUFFERS_2048
#define USE_CERT_BUFFERS_2048
#endif
#include <wolfssl/certs_test.h>
#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <mutex>
#include <new>

// Frontend (ps4_launcher.cpp), always linked.
void ps4_startup_checkpoint(const char* stage);

LOG_CHANNEL(tls_selftest_log, "TLS");

namespace
{
using steady = std::chrono::steady_clock;

constexpr char ca_bundle_path[] = "/app0/certs/cacert.pem"; // shipped by package-native.ps1; read by the download too
constexpr char test_host[] = "example.com";                 // a SAN of the wolfSSL test server certificate

enum : int
{
    state_idle = 0,
    state_running,
    state_passed,
    state_failed,
    state_not_run,
};

struct selftest_state
{
    std::atomic<int> state{state_idle};
    std::mutex mutex; // line
    std::string line;
};

// Never destroyed: the test thread may still run when the process ends.
selftest_state& state()
{
    static selftest_state* const s = new selftest_state();
    return *s;
}

u64 ms_since(steady::time_point start)
{
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(steady::now() - start).count());
}

// "YYYY-MM-DD HH:MM UTC" of the console clock; certificate dates are checked against it.
std::string clock_text()
{
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    if (!gmtime_r(&now, &utc)) return "unknown";
    char text[32]{};
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M UTC", &utc);
    return text;
}

// PEM text of one DER certificate (wolfSSL's own encoder is not built).
std::string der_to_pem(const unsigned char* der, std::size_t size)
{
    static constexpr char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string base64;
    base64.reserve((size + 2) / 3 * 4);
    for (std::size_t i = 0; i < size; i += 3)
    {
        const u32 b0 = der[i];
        const u32 b1 = i + 1 < size ? der[i + 1] : 0;
        const u32 b2 = i + 2 < size ? der[i + 2] : 0;
        const u32 v = (b0 << 16) | (b1 << 8) | b2;
        base64 += digits[(v >> 18) & 63];
        base64 += digits[(v >> 12) & 63];
        base64 += i + 1 < size ? digits[(v >> 6) & 63] : '=';
        base64 += i + 2 < size ? digits[v & 63] : '=';
    }
    std::string pem = "-----BEGIN CERTIFICATE-----\n";
    for (std::size_t at = 0; at < base64.size(); at += 64)
    {
        pem.append(base64, at, 64);
        pem += '\n';
    }
    pem += "-----END CERTIFICATE-----\n";
    return pem;
}

// One-connection HTTPS server on a listening loopback socket: TLS handshake with the wolfSSL test certificate,
// request read up to the empty line, fixed "ok" reply. Bounded: 6 s for the connection, 3 s per socket operation.
struct loopback_server
{
    int listen_fd;
    std::atomic<bool> stop{false}; // the client finished: stop waiting for a connection
    std::string error;             // "" when the reply was sent; written before the thread returns

    explicit loopback_server(int fd) noexcept
        : listen_fd(fd)
    {
    }

    loopback_server(const loopback_server&) = delete;
    loopback_server& operator=(const loopback_server&) = delete;

    void operator()()
    {
        try
        {
            serve();
        }
        catch (...)
        {
            error = "server: exception";
        }
    }

    void serve()
    {
        const steady::time_point deadline = steady::now() + std::chrono::seconds(6);
        int fd = -1;
        while (fd < 0)
        {
            if (stop.load() || thread_ctrl::state() == thread_state::aborting)
            {
                error = "server: no connection came";
                return;
            }
            if (steady::now() > deadline)
            {
                error = "server: accept timed out";
                return;
            }
            pollfd p{listen_fd, POLLIN, 0};
            const int ready = ::poll(&p, 1, 100);
            if (ready < 0 && errno != EINTR)
            {
                error = fmt::format("server: poll errno %d", errno);
                return;
            }
            if (ready <= 0) continue;
            fd = ::accept(listen_fd, nullptr, nullptr);
            if (fd < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            {
                error = fmt::format("server: accept errno %d", errno);
                return;
            }
        }

        timeval limit{};
        limit.tv_sec = 3;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));

        WOLFSSL_CTX* ctx = wolfSSL_CTX_new(wolfTLS_server_method());
        WOLFSSL* ssl = nullptr;
        if (!ctx)
            error = "server: CTX_new failed";
        else if (const int rc = wolfSSL_CTX_use_certificate_buffer(ctx, server_cert_der_2048, sizeof_server_cert_der_2048, WOLFSSL_FILETYPE_ASN1); rc != WOLFSSL_SUCCESS)
            error = fmt::format("server: certificate rc %d", rc);
        else if (const int rc = wolfSSL_CTX_use_PrivateKey_buffer(ctx, server_key_der_2048, sizeof_server_key_der_2048, WOLFSSL_FILETYPE_ASN1); rc != WOLFSSL_SUCCESS)
            error = fmt::format("server: key rc %d", rc);
        else if (ssl = wolfSSL_new(ctx); !ssl)
            error = "server: wolfSSL_new failed";
        else if (wolfSSL_set_fd(ssl, fd) != WOLFSSL_SUCCESS)
            error = "server: set_fd failed";
        else if (const int rc = wolfSSL_accept(ssl); rc != WOLFSSL_SUCCESS)
            error = fmt::format("server: handshake rc %d", wolfSSL_get_error(ssl, rc));
        else
        {
            char request[2048];
            std::size_t used = 0;
            bool complete = false;
            while (!complete && used < sizeof(request) - 1 && steady::now() < deadline)
            {
                const int got = wolfSSL_read(ssl, request + used, static_cast<int>(sizeof(request) - 1 - used));
                if (got <= 0) break;
                used += static_cast<std::size_t>(got);
                request[used] = '\0';
                complete = std::strstr(request, "\r\n\r\n") != nullptr;
            }
            static constexpr char reply[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
            constexpr int reply_size = static_cast<int>(sizeof(reply) - 1);
            if (!complete)
                error = fmt::format("server: request incomplete (%u bytes)", used);
            else if (wolfSSL_write(ssl, reply, reply_size) != reply_size)
                error = "server: reply not sent";
            else
                wolfSSL_shutdown(ssl);
        }
        if (ssl) wolfSSL_free(ssl);
        if (ctx) wolfSSL_CTX_free(ctx);
        ::close(fd);
    }
};

using server_thread = named_thread<loopback_server>;

struct client_transfer
{
    CURL* handle = nullptr;
    std::string body;
    std::string tls; // "TLSv1.3 TLS13-AES128-GCM-SHA256", read while the connection is open
};

std::size_t client_write(char* data, std::size_t size, std::size_t count, void* user) noexcept
{
    try
    {
        auto& t = *static_cast<client_transfer*>(user);
        if (t.tls.empty())
        {
            curl_tlssessioninfo* info = nullptr;
            if (curl_easy_getinfo(t.handle, CURLINFO_TLS_SSL_PTR, &info) == CURLE_OK && info && info->backend == CURLSSLBACKEND_WOLFSSL && info->internals)
            {
                WOLFSSL* ssl = static_cast<WOLFSSL*>(info->internals);
                const char* version = wolfSSL_get_version(ssl);
                const char* cipher = wolfSSL_get_cipher_name(ssl);
                t.tls = fmt::format("%s %s", version ? version : "?", cipher ? cipher : "?");
            }
        }
        const std::size_t bytes = size * count;
        if (t.body.size() + bytes > 256) return 0;
        t.body.append(data, bytes);
        return bytes;
    }
    catch (...)
    {
        return 0;
    }
}

// Step 5: one HTTPS GET over loopback. Returns the part of the line and sets 'ok'.
std::string run_loopback(bool& ok, bool& not_run)
{
    ok = false;
    not_run = false;

    const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) return fmt::format("loopback: socket errno %d", errno);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    socklen_t length = sizeof(address);
    if (::bind(listen_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 || ::listen(listen_fd, 1) != 0 ||
        ::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&address), &length) != 0)
    {
        const int error = errno;
        ::close(listen_fd);
        return fmt::format("loopback: bind/listen errno %d", error);
    }
    const u16 port = ntohs(address.sin_port);

    server_thread* server = new (std::nothrow) server_thread(stx::try_launch, "TLS Self-test Server", listen_fd);
    if (!server || !server->ps4_try_start())
    {
        delete server; // never started: no join
        ::close(listen_fd);
        not_run = true;
        return "loopback not run (server thread refused)";
    }

    const std::string url = fmt::format("https://%s:%u/selftest", test_host, port);
    const std::string resolve_line = fmt::format("%s:%u:127.0.0.1", test_host, port);
    std::string ca = der_to_pem(ca_cert_der_2048, sizeof_ca_cert_der_2048);
    curl_blob blob{ca.data(), ca.size(), CURL_BLOB_COPY};
    char error_text[CURL_ERROR_SIZE]{};
    client_transfer transfer;
    long http = 0;
    CURLcode result = CURLE_FAILED_INIT;
    const steady::time_point start = steady::now();

    if (CURL* curl = curl_easy_init())
    {
        curl_slist* resolve = curl_slist_append(nullptr, resolve_line.c_str());
        transfer.handle = curl;
        // The options of the patch download (utils::ps4::patches::download), with this test CA as the only root
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve);
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2));
        curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &blob);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_text);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &client_write);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
        result = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
        curl_easy_cleanup(curl);
        curl_slist_free_all(resolve);
    }
    const u64 elapsed = ms_since(start);

    // Join (the server is bounded by its own timeouts), then read its result
    server->stop = true;
    (*server)();
    const std::string server_error = server->error;
    delete server;
    ::close(listen_fd);

    ok = result == CURLE_OK && http == 200 && transfer.body == "ok";

    std::string body = transfer.body;
    for (char& c : body)
        if (static_cast<unsigned char>(c) < 0x20 || c == '\'') c = '?';
    std::string text = fmt::format("loopback https://%s:%u -> 127.0.0.1: curl %d", test_host, port, static_cast<int>(result));
    if (result != CURLE_OK)
        text += fmt::format(" '%s' [%s]", error_text, curl_easy_strerror(result));
    text += fmt::format(", HTTP %d '%s'", http, body);
    if (!transfer.tls.empty()) text += ", " + transfer.tls;
    text += fmt::format(", %u ms", elapsed);
    if (!ok && !server_error.empty()) text += "; " + server_error;
    return text;
}

struct selftest_worker
{
    selftest_worker() noexcept = default;
    selftest_worker(const selftest_worker&) = delete;
    selftest_worker& operator=(const selftest_worker&) = delete;

    void operator()();
};

void selftest_worker::operator()()
{
    std::string line = "TLS self-test: ";
    bool passed = true;
    bool inconclusive = false;
    std::string failed_step;

    try
    {
        const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
        line += fmt::format("libcurl %s %s", info && info->version ? info->version : "?", info && info->ssl_version ? info->ssl_version : "(no TLS backend)");

        // 2. The multi handle curl_easy_perform creates first (it failed with eventfd() unavailable)
        if (CURLM* multi = curl_multi_init())
        {
            curl_multi_cleanup(multi);
            line += "; multi ok";
        }
        else
        {
            line += "; multi NULL";
            passed = false;
            failed_step = "multi";
        }

        // 3. wolfSSL DRBG, seeded from libSceRandom
        {
            WC_RNG rng{};
            unsigned char block[32]{};
            int rc = wc_InitRng(&rng);
            if (rc == 0)
            {
                rc = wc_RNG_GenerateBlock(&rng, block, sizeof(block));
                wc_FreeRng(&rng);
            }
            if (rc == 0)
                line += "; RNG ok";
            else
            {
                line += fmt::format("; RNG rc %d", rc);
                passed = false;
                if (failed_step.empty()) failed_step = "RNG";
            }
        }

        // 4. CA bundle, loaded root by root the way curl loads CURLOPT_CAINFO_BLOB (wolfSSL_CTX_load_verify_buffer, PEM)
        {
            std::string pem;
            const fs::file file(ca_bundle_path);
            if (file)
            {
                pem.resize(static_cast<std::size_t>(file.size()));
                if (!pem.empty() && file.read(pem.data(), pem.size()) != pem.size()) pem.clear();
            }
            const steady::time_point start = steady::now();
            WOLFSSL_CTX* ctx = pem.empty() ? nullptr : wolfSSL_CTX_new(wolfTLS_client_method());
            u32 roots = 0, accepted = 0, expired = 0, early = 0, other = 0;
            int first_other_rc = 0;
            if (ctx)
            {
                static constexpr std::string_view begin = "-----BEGIN CERTIFICATE-----";
                static constexpr std::string_view end = "-----END CERTIFICATE-----";
                std::size_t at = 0;
                while ((at = pem.find(begin, at)) != std::string::npos)
                {
                    const std::size_t stop = pem.find(end, at);
                    if (stop == std::string::npos) break;
                    const std::size_t size = stop + end.size() - at;
                    ++roots;
                    const int rc = wolfSSL_CTX_load_verify_buffer(ctx, reinterpret_cast<const unsigned char*>(pem.data() + at), static_cast<long>(size), WOLFSSL_FILETYPE_PEM);
                    if (rc == WOLFSSL_SUCCESS)
                        ++accepted;
                    else if (rc == ASN_AFTER_DATE_E)
                        ++expired;
                    else if (rc == ASN_BEFORE_DATE_E)
                        ++early;
                    else if (!other++)
                        first_other_rc = rc;
                    at = stop + end.size();
                }
                wolfSSL_CTX_free(ctx);
            }
            const u64 elapsed = ms_since(start);
            if (pem.empty())
            {
                line += fmt::format("; CA bundle absent (%s)", ca_bundle_path);
                passed = false;
                if (failed_step.empty()) failed_step = "CA bundle";
            }
            else if (!ctx)
            {
                line += "; CA load: CTX_new failed";
                passed = false;
                if (failed_step.empty()) failed_step = "CA load";
            }
            else
            {
                // curl accepts the bundle when at least one root loads; wolfSSL refuses an out-of-date root by itself
                line += fmt::format("; CA bundle %u roots in %u ms", roots, elapsed);
                if (accepted != roots)
                {
                    std::string refused;
                    if (expired) refused += fmt::format(", %u expired", expired);
                    if (early) refused += fmt::format(", %u not yet valid", early);
                    if (other) refused += fmt::format(", %u refused (first CA load rc %d)", other, first_other_rc);
                    line += fmt::format(" (%u accepted%s)", accepted, refused);
                }
                if (!accepted)
                {
                    passed = false;
                    if (failed_step.empty()) failed_step = "CA load";
                }
            }
        }

        // 5. Loopback HTTPS
        {
            bool ok = false, not_run = false;
            line += "; " + run_loopback(ok, not_run);
            if (not_run)
                inconclusive = true;
            else if (!ok)
            {
                passed = false;
                if (failed_step.empty()) failed_step = "loopback";
            }
        }
    }
    catch (...)
    {
        line += "; exception";
        passed = false;
        if (failed_step.empty()) failed_step = "exception";
    }

    int verdict = state_passed;
    if (!passed)
    {
        verdict = state_failed;
        line += fmt::format(" -> TLS FAILED at %s (console clock %s; no internet used)", failed_step, clock_text());
    }
    else if (inconclusive)
    {
        verdict = state_not_run;
        line += " -> TLS NOT TESTED (no internet used)";
    }
    else
        line += " -> TLS OK (no internet used)";

    ps4_startup_checkpoint(line.c_str());
    if (verdict == state_passed)
        tls_selftest_log.notice("%s", line);
    else
        tls_selftest_log.error("%s", line);

    auto& s = state();
    {
        std::lock_guard lock(s.mutex);
        s.line = std::move(line);
    }
    s.state.store(verdict, std::memory_order_release);
}

// 0.39: the worker runs on a detached native thread, which the kernel reclaims when it ends (0.38: a named_thread that
// was never joined held one slot of the per-process thread cap for the whole process).
void* selftest_entry(void*)
{
    try
    {
        selftest_worker worker;
        worker();
    }
    catch (...)
    {
    }
    return nullptr;
}
} // namespace

void ps4_tls_selftest_start() noexcept
{
    auto& s = state();
    int expected = state_idle;
    if (!s.state.compare_exchange_strong(expected, state_running)) return;

    try
    {
        // Once, on the main thread: curl_easy_init would otherwise run it lazily on whichever thread comes first
        if (const CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT); rc != CURLE_OK)
            ps4_startup_checkpoint(fmt::format("TLS self-test: curl_global_init returned %d", static_cast<int>(rc)).c_str());

        // Detached: nothing joins it, and the process may end while it runs
        pthread_t thread{};
        if (::pthread_create(&thread, nullptr, &selftest_entry, nullptr) == 0)
        {
            ::pthread_detach(thread);
            return;
        }

        const std::string line = "TLS self-test: not run (thread refused) -> TLS NOT TESTED";
        ps4_startup_checkpoint(line.c_str());
        std::lock_guard lock(s.mutex);
        s.line = line;
    }
    catch (...)
    {
    }
    s.state.store(state_not_run, std::memory_order_release);
}

std::optional<bool> ps4_tls_selftest_ok() noexcept
{
    switch (state().state.load(std::memory_order_acquire))
    {
    case state_passed: return true;
    case state_failed: return false;
    default: return std::nullopt;
    }
}

std::string ps4_tls_selftest_line() noexcept
{
    try
    {
        auto& s = state();
        const int current = s.state.load(std::memory_order_acquire);
        if (current == state_idle || current == state_running) return {};
        std::lock_guard lock(s.mutex);
        return s.line;
    }
    catch (...)
    {
        return {};
    }
}

#else

void ps4_tls_selftest_start() noexcept
{
}

std::optional<bool> ps4_tls_selftest_ok() noexcept
{
    return std::nullopt;
}

std::string ps4_tls_selftest_line() noexcept
{
    return {};
}

#endif // RPCS3_PS4
