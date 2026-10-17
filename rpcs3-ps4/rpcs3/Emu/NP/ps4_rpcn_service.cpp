#include "stdafx.h"
#include "ps4_rpcn_service.h"

#ifdef RPCS3_PS4

// 0.31: RPCN account service for the XMB launcher. Built in the NP target (exceptions on, wolfSSL ABI defines);
// the launcher only sees ps4_rpcn_service.h, and no exception leaves the functions declared there.
//
// Each job runs on its own "RPCN Account" thread, one job at a time, and uses a fresh rpcn_client: a client keeps
// its first failure for good (wait_for_connection returns the stored state), and its destructor joins three
// threads. Creating and dropping the client on the job thread keeps both the waits and the joins off the XMB
// thread, which has to keep presenting frames.
//
// rpcn.yml lives in g_cfg_rpcn, which the rpcn_client threads read without a lock (its constructor also reloads
// it). So this file touches g_cfg_rpcn only under the service mutex and never while one of its clients is alive.
// The XMB-thread functions (poll, account_line, hosts, current_host, cycle_host) read a snapshot taken after each
// job and only load the file themselves while no job runs.

#include "rpcn_client.h"
#include "rpcn_config.h"
#include "Emu/system_config.h"
#include "Utilities/File.h"
#include "Utilities/Thread.h"
#include "util/ps4_i18n.hpp"
#include "util/ps4_memory_governor.hpp"
#include "util/ps4_rpcn_account.hpp"

// libSceNet/libSceNetCtl start-up, resolver and probes shared with rpcn_client. This file declares no SCE function
// itself: that header binds them weakly under its own names, and a second, strong declaration of the same symbols
// here would conflict with it.
#if __has_include("util/ps4_net.hpp")
#include "util/ps4_net.hpp"
#define RPCS3_PS4_RPCN_NET 1
#endif

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wold-style-cast"
#pragma clang diagnostic ignored "-Wextern-c-compat"
#endif
#include <wolfssl/wolfcrypt/random.h>
#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

// Frontend (ps4_launcher.cpp), always linked.
void ps4_startup_checkpoint(const char* stage);

LOG_CHANNEL(rpcn_account_log, "RPCN Account");

namespace ps4_rpcn
{
namespace
{
namespace acc = utils::ps4::rpcn_account;
namespace i18n = utils::ps4::i18n;
using i18n::str;

// Status texts in the interface language of the moment they are set
constexpr str msg_no_resources = str::RPCNSVC_SEM_RECURSOS_CONECTAR_AGORA;
constexpr str msg_internal = str::RPCNSVC_ERRO_INTERNO_OPERACAO_RPCN_DETALHES;
constexpr str msg_no_account = str::RPCNSVC_NENHUMA_CONTA_RPCN_ENTRE_CRIE;
constexpr str msg_not_saved = str::RPCNSVC_NAO_FOI_POSSIVEL_SALVAR;
constexpr char data_dir[] = "/data/rpcs3/";
// Console test: makes start() behave as if the console refused the job thread. Read once.
constexpr char refuse_flag[] = "/data/rpcs3/test-refuse-rpcn-account.flag";
// The XMB "Modo online" switch (online_rpcn_flag in util/ps4_policy.hpp); only reported by the diagnostics.
constexpr char online_flag[] = "/data/rpcs3/online-rpcn.flag";

struct request
{
    std::string user, pass, email, token;

    void wipe() noexcept
    {
        acc::wipe(user);
        acc::wipe(pass);
        acc::wipe(email);
        acc::wipe(token);
    }
};

// Wipes a string holding a credential on every way out of a scope, exceptions included.
struct wipe_guard
{
    std::string& text;
    ~wipe_guard() { acc::wipe(text); }
};

struct snapshot
{
    bool loaded = false;
    bool has_password = false;
    std::string npid, host;
    std::vector<std::string> hosts;
};

struct account_worker
{
    job type;
    request req;

    account_worker(job t, request&& r) noexcept
        : type(t), req(std::move(r))
    {
    }

    account_worker(const account_worker&) = delete;
    account_worker& operator=(const account_worker&) = delete;

    void operator()();
};

using worker_thread = named_thread<account_worker>;

struct service_state
{
    std::mutex mutex; // everything below, and every g_cfg_rpcn access made by this file
    status current;
    snapshot snap;
    worker_thread* worker = nullptr; // last started job thread; deleted (joined) by the next start()
    std::chrono::steady_clock::time_point started{}; // when the running job started
};

// Never destroyed: a job may still be running when the process ends.
service_state& state()
{
    static service_state* const s = new service_state();
    return *s;
}

[[gnu::format(printf, 1, 2)]] void checkpoint(const char* format, ...)
{
    char line[384];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    ps4_startup_checkpoint(line);
    rpcn_account_log.notice("%s", line);
}

const char* job_name(job type)
{
    switch (type)
    {
    case job::test: return "test";
    case job::login: return "login";
    case job::create: return "create";
    case job::token: return "token";
    case job::resend: return "resend";
    case job::import_yml: return "import_yml";
    case job::diag: return "diag";
    }
    return "unknown";
}

const char* progress_text(job type)
{
    switch (type)
    {
    case job::test: return i18n::tr(str::COM_CONECTANDO_RPCN);
    case job::login: return i18n::tr(str::RPCNSVC_ENTRANDO_RPCN);
    case job::create: return i18n::tr(str::RPCNSVC_CRIANDO_CONTA_RPCN);
    case job::token: return i18n::tr(str::RPCNSVC_VALIDANDO_TOKEN);
    case job::resend: return i18n::tr(str::RPCNSVC_PEDINDO_NOVO_TOKEN);
    case job::import_yml: return i18n::tr(str::RPCNSVC_IMPORTANDO_RPCN_YML);
    case job::diag: return i18n::tr(str::RPCNSVC_DIAGNOSTICO_REDE);
    }
    return i18n::tr(str::COM_CONECTANDO_RPCN);
}

// Jobs that create an rpcn_client, i.e. three more threads besides the job thread.
bool needs_client(job type)
{
    return type != job::import_yml && type != job::diag;
}

std::string state_text(rpcn::rpcn_state result)
{
    using rpcn::rpcn_state;
    switch (result)
    {
    case rpcn_state::failure_no_failure: return i18n::tr(str::RPCNSVC_CONECTADO);
    case rpcn_state::failure_input: return i18n::tr(str::RPCNSVC_ENDERECO_SERVIDOR_RPCN_INVALIDO);
    case rpcn_state::failure_wolfssl: return i18n::tr(str::RPCNSVC_ERRO_TLS_WOLFSSL_CONECTAR_RPCN);
    case rpcn_state::failure_resolve: return i18n::tr(str::RPCNSVC_NAO_FOI_POSSIVEL_ENCONTRAR_SERVIDOR);
    case rpcn_state::failure_binding: return i18n::tr(str::RPCNSVC_FALHA_ABRIR_CONEXAO_LOCAL);
    case rpcn_state::failure_connect: return i18n::tr(str::RPCNSVC_NAO_FOI_POSSIVEL_CONECTAR_SERVIDOR);
    case rpcn_state::failure_id: return i18n::tr(str::RPCNSVC_ERRO_IDENTIFICACAO_LOGIN_RPCN);
    case rpcn_state::failure_id_already_logged_in: return i18n::tr(str::RPCNSVC_ESTA_CONTA_RPCN_JA_ESTA);
    case rpcn_state::failure_id_username: return i18n::tr(str::RPCNSVC_USUARIO_RPCN_NAO_ENCONTRADO);
    case rpcn_state::failure_id_password: return i18n::tr(str::RPCNSVC_SENHA_RPCN_INCORRETA);
    case rpcn_state::failure_id_token: return i18n::tr(str::RPCNSVC_TOKEN_RPCN_INVALIDO_AINDA_NAO);
    case rpcn_state::failure_protocol: return i18n::tr(str::RPCNSVC_VERSAO_RPCN_RECUSADA_SERVIDOR_RPCS3);
    case rpcn_state::failure_other: return i18n::tr(str::RPCNSVC_ERRO_CONEXAO_RPCN);
    }
    return i18n::tr(str::RPCNSVC_ERRO_CONEXAO_RPCN);
}

std::string creation_error_text(rpcn::ErrorType error)
{
    using rpcn::ErrorType;
    switch (error)
    {
    case ErrorType::CreationExistingUsername: return i18n::tr(str::RPCNSVC_JA_EXISTE_CONTA_ESSE_NOME);
    case ErrorType::CreationBannedEmailProvider: return i18n::tr(str::RPCNSVC_ESSE_PROVEDOR_EMAIL_BLOQUEADO_RPCN);
    case ErrorType::CreationExistingEmail: return i18n::tr(str::RPCNSVC_JA_EXISTE_CONTA_ESSE);
    case ErrorType::CreationError: return i18n::tr(str::RPCNSVC_ERRO_CRIAR_CONTA_RPCN);
    case ErrorType::InvalidInput: return i18n::tr(str::RPCNSVC_DADOS_CONTA_RECUSADOS_SERVIDOR);
    default: return i18n::trf(str::RPCNSVC_ERRO_DESCONHECIDO_CRIAR_CONTA_CODIGO, {static_cast<u32>(error)});
    }
}

std::string resend_error_text(rpcn::ErrorType error)
{
    using rpcn::ErrorType;
    switch (error)
    {
    case ErrorType::Invalid: return i18n::tr(str::RPCNSVC_ESTE_SERVIDOR_NAO_USA);
    case ErrorType::DbFail: return i18n::tr(str::RPCNSVC_ERRO_BANCO_DADOS_SERVIDOR);
    case ErrorType::TooSoon: return i18n::tr(str::RPCNSVC_SO_POSSIVEL_PEDIR_TOKEN_CADA);
    case ErrorType::EmailFail: return i18n::tr(str::RPCNSVC_SERVIDOR_NAO_CONSEGUIU_ENVIAR);
    case ErrorType::LoginError: return i18n::tr(str::RPCNSVC_USUARIO_SENHA_RPCN_INVALIDOS);
    case ErrorType::LoginAlreadyLoggedIn: return i18n::tr(str::RPCNSVC_CONTA_JA_CONECTADA_TOKEN_NAO);
    case ErrorType::Malformed: return i18n::tr(str::RPCNSVC_SEM_RESPOSTA_SERVIDOR_RPCN);
    default: return i18n::trf(str::RPCNSVC_ERRO_DESCONHECIDO_PEDIR_TOKEN_CODIGO, {static_cast<u32>(error)});
    }
}

std::string config_path()
{
    return fs::get_config_dir(true) + "rpcn.yml";
}

bool saved_password_locked()
{
    std::string password = g_cfg_rpcn.get_password();
    const bool present = !password.empty();
    acc::wipe(password);
    return present;
}

// Reads g_cfg_rpcn (already loaded) into the snapshot. Reads the fields directly: get_npid() would invent and save
// a random NPID when none is set, and get_hosts() would save the default list.
void refresh_snapshot_locked(snapshot& snap)
{
    snap.loaded = true;
    snap.npid = g_cfg_rpcn.npid.to_string();
    snap.has_password = saved_password_locked();
    snap.host = g_cfg_rpcn.get_host();
    snap.hosts.clear();
    for (auto& entry : acc::parse_hosts(g_cfg_rpcn.hosts.to_string()))
        snap.hosts.push_back(std::move(entry.address));
    if (snap.hosts.empty()) snap.hosts.push_back(snap.host);
}

void load_config_locked(snapshot& snap)
{
    g_cfg_rpcn.load();
    refresh_snapshot_locked(snap);
}

void ensure_snapshot_locked(service_state& s)
{
    if (!s.snap.loaded && !s.current.busy) load_config_locked(s.snap);
}

// Writes g_cfg_rpcn the way cfg_rpcn::save() does (temporary file, then rename over rpcn.yml), but reports the
// result, and refuses any directory other than /data/rpcs3/.
bool save_config_locked()
{
    const std::string path = config_path();
    if (!path.starts_with(data_dir))
    {
        rpcn_account_log.error("rpcn.yml path is not under %s: %s", data_dir, path);
        return false;
    }
    if (static_cast<const cfg::_base&>(g_cfg_rpcn).save(path)) return true;
    rpcn_account_log.error("Could not save %s (error=%s)", path, fs::g_tls_error);
    return false;
}

// Small whole-file read. 1: read, 0: missing, -1: unreadable (errno says why), -2: larger than 'limit'.
int read_small_file(const char* path, std::string& out, std::size_t limit)
{
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) return errno == ENOENT || errno == ENOTDIR ? 0 : -1;
    struct stat st{};
    const bool stat_ok = ::fstat(fd, &st) == 0;
    if (!stat_ok || !S_ISREG(st.st_mode))
    {
        // close() may change errno; keep the reason for the log line.
        const int error = !stat_ok ? errno : S_ISDIR(st.st_mode) ? EISDIR : EINVAL;
        ::close(fd);
        errno = error;
        return -1;
    }
    if (st.st_size < 0 || static_cast<u64>(st.st_size) > limit)
    {
        ::close(fd);
        return -2;
    }
    out.assign(static_cast<std::size_t>(st.st_size), '\0');
    std::size_t done = 0;
    int error = 0;
    while (done < out.size())
    {
        const auto n = ::read(fd, out.data() + done, out.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0)
        {
            error = n < 0 ? errno : EIO;
            break;
        }
        done += static_cast<std::size_t>(n);
    }
    ::close(fd);
    if (error)
    {
        // A cut-off rpcn.yml (USB pulled, read error) must not be imported or kept as the backup.
        errno = error;
        return -1;
    }
    return 1;
}

bool write_file_atomic(const std::string& path, std::string_view text)
{
    if (!path.starts_with(data_dir)) return false;
    fs::pending_file file(path);
    if (!file.file) return false;
    if (file.file.write(text.data(), text.size()) != text.size()) return false;
    return file.commit();
}

void set_progress(const char* message)
{
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.current.message = message;
}

void finish(job type, bool ok, std::string message) noexcept
{
    try
    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        // The job's client is gone, so g_cfg_rpcn is quiet again.
        refresh_snapshot_locked(s.snap);
        s.current.busy = false;
        s.current.ok = ok;
        s.current.message = std::move(message);
        s.current.npid = s.snap.npid;
        rpcn_account_log.notice("job %s finished: %s (%s)", job_name(type), ok ? "ok" : "failed", s.current.message);
    }
    catch (...)
    {
        // Last resort: a job must never stay busy.
        auto& s = state();
        try
        {
            std::lock_guard lock(s.mutex);
            s.current.busy = false;
            s.current.ok = false;
        }
        catch (...)
        {
            s.current.busy = false;
            s.current.ok = false;
        }
    }
}

bool refuse_flag_set()
{
    static const bool present = []
    {
        struct stat st{};
        const bool found = ::stat(refuse_flag, &st) == 0;
        if (found) checkpoint("RPCN account: %s present: job threads are refused (test)", refuse_flag);
        return found;
    }();
    return present;
}

// libSceNet / libSceNetCtl start-up (once per process). Returns the results for the diagnostics.
std::string net_prepare()
{
#ifdef RPCS3_PS4_RPCN_NET
    const auto& s = utils::ps4::net::prepare();
    return fmt::format("net module 0x%x, sceNetInit 0x%x, netctl module 0x%x, sceNetCtlInit 0x%x, resolver pool %d",
        static_cast<u32>(s.net_module), static_cast<u32>(s.net_init), static_cast<u32>(s.netctl_module),
        static_cast<u32>(s.netctl_init), s.pool);
#else
    return "network helpers not in this build";
#endif
}

// Connects with a fresh client (its constructor reloads rpcn.yml), logs in when asked, runs 'body', and drops the
// client on this thread. Returns false with 'message' set when any step fails.
template <typename F>
bool with_client(bool login, std::string& message, F&& body)
{
    std::shared_ptr<rpcn::rpcn_client> client;
    try
    {
        client = rpcn::rpcn_client::get_instance(0);
    }
    catch (const std::exception& e)
    {
        rpcn_account_log.error("RPCN client not created: %s", e.what());
        message = i18n::tr(msg_no_resources);
        return false;
    }

    if (!client)
    {
        message = i18n::tr(msg_no_resources);
        return false;
    }

    rpcn::rpcn_state result = client->wait_for_connection();
    if (result == rpcn::rpcn_state::failure_no_failure && login)
    {
        result = client->wait_for_authentified();
    }

    bool ok = false;
    if (result != rpcn::rpcn_state::failure_no_failure)
    {
        message = state_text(result);
        rpcn_account_log.warning("RPCN %s failed: %s", login ? "login" : "connection", rpcn::rpcn_state_to_string(result));
    }
    else
    {
        ok = body(*client, message);
    }

    client.reset();
    return ok;
}

std::string derive_timed(std::string_view typed)
{
    const auto t0 = std::chrono::steady_clock::now();
    std::string derived = acc::derive_password(typed);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    if (derived.empty())
        rpcn_account_log.error("Password key derivation failed after %d ms", static_cast<s64>(ms));
    else
        rpcn_account_log.notice("Password key derived in %d ms", static_cast<s64>(ms));
    return derived;
}

bool job_test(std::string& message)
{
    std::string npid;
    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        load_config_locked(s.snap);
        if (s.snap.npid.empty() || !s.snap.has_password)
        {
            message = i18n::tr(msg_no_account);
            return false;
        }
        npid = s.snap.npid;
    }

    set_progress(i18n::tr(str::COM_CONECTANDO_RPCN));
    if (!with_client(true, message, [](rpcn::rpcn_client&, std::string&) { return true; }))
    {
        return false;
    }

    message = i18n::trf(str::RPCNSVC_CONNECTED_AS, {npid});
    return true;
}

bool job_login(request& req, std::string& message)
{
    set_progress(i18n::tr(str::RPCNSVC_CALCULANDO_CHAVE_SENHA));
    std::string derived = derive_timed(req.pass);
    wipe_guard derived_guard{derived};
    acc::wipe(req.pass);
    if (derived.empty())
    {
        message = i18n::tr(str::RPCNSVC_FALHA_CALCULAR_CHAVE_SENHA);
        return false;
    }

    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        g_cfg_rpcn.load();
        const bool same_account = g_cfg_rpcn.npid.to_string() == req.user;
        g_cfg_rpcn.set_npid(req.user);
        g_cfg_rpcn.set_password(derived);
        if (!req.token.empty())
            g_cfg_rpcn.set_token(req.token);
        else if (!same_account)
            g_cfg_rpcn.set_token(""); // the saved token belongs to the previous account
        const bool saved = save_config_locked();
        refresh_snapshot_locked(s.snap);
        if (!saved)
        {
            message = i18n::tr(msg_not_saved);
            return false;
        }
    }

    checkpoint("RPCN account: credentials saved for %s", req.user.c_str());
    return job_test(message);
}

bool job_create(request& req, std::string& message)
{
    set_progress(i18n::tr(str::RPCNSVC_CALCULANDO_CHAVE_SENHA));
    std::string derived = derive_timed(req.pass);
    wipe_guard derived_guard{derived};
    acc::wipe(req.pass);
    if (derived.empty())
    {
        message = i18n::tr(str::RPCNSVC_FALHA_CALCULAR_CHAVE_SENHA);
        return false;
    }

    std::string host;
    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        load_config_locked(s.snap);
        host = s.snap.host;
    }

    set_progress(i18n::tr(str::RPCNSVC_CRIANDO_CONTA_RPCN));
    checkpoint("RPCN account: creating account %s on %s", req.user.c_str(), host.c_str());
    const bool created = with_client(false, message, [&](rpcn::rpcn_client& client, std::string& text)
    {
        const rpcn::ErrorType error = client.create_user(req.user, derived, req.user, acc::default_avatar, req.email);
        if (error == rpcn::ErrorType::NoError) return true;
        text = creation_error_text(error);
        rpcn_account_log.error("Account creation refused by the server (error %u)", static_cast<u32>(error));
        return false;
    });

    if (!created)
    {
        return false;
    }

    bool saved = false;
    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        g_cfg_rpcn.set_npid(req.user);
        g_cfg_rpcn.set_password(derived);
        g_cfg_rpcn.set_token("");
        saved = save_config_locked();
        refresh_snapshot_locked(s.snap);
    }

    checkpoint("RPCN account: account %s created (%s)", req.user.c_str(), saved ? "saved" : "rpcn.yml NOT saved");
    message = i18n::trf(saved ? str::RPCNSVC_ACCOUNT_CREATED : str::RPCNSVC_ACCOUNT_CREATED_NOT_SAVED, {req.user});
    return saved;
}

bool job_token(request& req, std::string& message)
{
    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        load_config_locked(s.snap);
        if (s.snap.npid.empty() || !s.snap.has_password)
        {
            acc::wipe(req.token);
            message = i18n::tr(msg_no_account);
            return false;
        }
        g_cfg_rpcn.set_token(req.token);
        acc::wipe(req.token);
        const bool saved = save_config_locked();
        refresh_snapshot_locked(s.snap);
        if (!saved)
        {
            message = i18n::tr(msg_not_saved);
            return false;
        }
    }

    checkpoint("RPCN account: token saved");
    return job_test(message);
}

bool job_resend(std::string& message)
{
    std::string npid, password;
    wipe_guard password_guard{password};
    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        load_config_locked(s.snap);
        if (s.snap.npid.empty() || !s.snap.has_password)
        {
            message = i18n::tr(msg_no_account);
            return false;
        }
        npid = s.snap.npid;
        password = g_cfg_rpcn.get_password();
    }

    set_progress(i18n::tr(str::RPCNSVC_PEDINDO_NOVO_TOKEN));
    const bool ok = with_client(false, message, [&](rpcn::rpcn_client& client, std::string& text)
    {
        const rpcn::ErrorType error = client.resend_token(npid, password);
        if (error == rpcn::ErrorType::NoError) return true;
        text = resend_error_text(error);
        rpcn_account_log.error("Token resend refused (error %u)", static_cast<u32>(error));
        return false;
    });

    if (ok)
    {
        message = i18n::trf(str::RPCNSVC_TOKEN_RESENT, {npid});
    }
    return ok;
}

bool job_import(std::string& message)
{
    static constexpr const char* sources[] = {
        "/mnt/usb0/rpcs3/rpcn.yml",
        "/mnt/usb1/rpcs3/rpcn.yml",
        "/mnt/usb0/rpcn.yml",
        "/mnt/usb1/rpcn.yml",
        "/data/rpcs3/import/rpcn.yml",
    };

    std::string text;
    wipe_guard text_guard{text};
    const char* from = nullptr;
    for (const char* path : sources)
    {
        const int r = read_small_file(path, text, acc::import_max_bytes);
        if (r == 0) continue;
        if (r == -2)
        {
            checkpoint("RPCN account: import: %s is larger than %u bytes, ignored", path, static_cast<u32>(acc::import_max_bytes));
            message = i18n::trf(str::RPCNSVC_YML_TOO_LARGE, {path});
            return false;
        }
        if (r < 0)
        {
            checkpoint("RPCN account: import: %s exists but could not be read (errno %d)", path, errno);
            continue;
        }
        from = path;
        break;
    }

    if (!from)
    {
        message = i18n::tr(str::RPCNSVC_RPCN_YML_NAO_ENCONTRADO_USE);
        return false;
    }

    cfg_rpcn imported;
    imported.from_default();
    if (!imported.from_string(text))
    {
        checkpoint("RPCN account: import: %s is not a valid rpcn.yml", from);
        message = i18n::tr(str::RPCNSVC_RPCN_YML_INVALIDO_NAO_YAML);
        return false;
    }

    std::string password = imported.get_password();
    wipe_guard password_guard{password};
    const std::string npid = imported.npid.to_string();
    if (const std::string_view why = acc::check_import(npid, password, static_cast<unsigned>(imported.version.get())); !why.empty())
    {
        checkpoint("RPCN account: import from %s refused: %s", from, std::string(why).c_str());
        message = std::string(why);
        return false;
    }

    // The IPv6 path of the P2P port is not usable on the console; a PC file may have it on.
    imported.set_ipv6_support(false);
    std::string out = imported.to_string();
    wipe_guard out_guard{out};

    auto& s = state();
    std::lock_guard lock(s.mutex);
    const std::string dest = config_path();
    if (!dest.starts_with(data_dir))
    {
        rpcn_account_log.error("rpcn.yml path is not under %s: %s", data_dir, dest);
        message = i18n::tr(msg_not_saved);
        return false;
    }

    // Keep the current file as rpcn.yml.bak before replacing it.
    std::string old;
    wipe_guard old_guard{old};
    if (const int r = read_small_file(dest.c_str(), old, 1024 * 1024); r == 1)
    {
        if (!write_file_atomic(dest + ".bak", old))
        {
            checkpoint("RPCN account: import: could not write %s.bak, rpcn.yml left unchanged", dest.c_str());
            message = i18n::tr(str::RPCNSVC_NAO_FOI_POSSIVEL_SALVAR_COPIA);
            return false;
        }
    }

    if (!write_file_atomic(dest, out))
    {
        checkpoint("RPCN account: import: could not write %s (error %s)", dest.c_str(), fmt::format("%s", fs::g_tls_error).c_str());
        message = i18n::tr(msg_not_saved);
        return false;
    }

    load_config_locked(s.snap);
    checkpoint("RPCN account: imported %s (NPID %s, server %s)", from, npid.c_str(), s.snap.host.c_str());
    message = i18n::trf(str::RPCNSVC_IMPORTED, {npid});
    return true;
}

std::string ipv4_text(const void* data)
{
    const auto* b = static_cast<const unsigned char*>(data);
    char text[16];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return text;
}

// NetCtl state, DNS settings (sceNetGetDnsInfo, whose return value OpenOrbis' musl resolver reads as the number of
// DNS servers, and the NetCtl strings), the IPPROTO_IP numbering probe, and whether the PS3 P2P port is free.
void diag_net()
{
#ifdef RPCS3_PS4_RPCN_NET
    checkpoint("rpcn diag: netctl state %d (3 = IP address obtained)", utils::ps4::net::link_state());
    utils::ps4::net::dns_report();      // writes its own startup.log line
    utils::ps4::net::probe_numbering(); // idem
    const int bind_error = utils::ps4::net::udp_bind_probe(3658);
    checkpoint("rpcn diag: UDP bind 0.0.0.0:3658 %s (errno %d)", bind_error ? "failed" : "ok, the port is free", bind_error);
#else
    checkpoint("rpcn diag: network helpers not in this build, NetCtl/DNS/socket probes skipped");
#endif
}

void diag_entropy()
{
    const int fd = ::open("/dev/urandom", O_RDONLY);
    const int open_errno = fd < 0 ? errno : 0;
    long got = -1;
    if (fd >= 0)
    {
        unsigned char bytes[16]{};
        got = static_cast<long>(::read(fd, bytes, sizeof(bytes)));
        acc::wipe(bytes, sizeof(bytes));
        ::close(fd);
    }
    checkpoint("rpcn diag: open(/dev/urandom) fd %d errno %d, read %ld of 16 bytes", fd, open_errno, got);

    WC_RNG rng{};
    const int init = wc_InitRng(&rng);
    int block = -1;
    if (init == 0)
    {
        byte bytes[16]{};
        block = wc_RNG_GenerateBlock(&rng, bytes, static_cast<word32>(sizeof(bytes)));
        acc::wipe(bytes, sizeof(bytes));
        wc_FreeRng(&rng);
    }
    checkpoint("rpcn diag: wolfSSL RNG init %d, generate %d (0 = TLS can start)", init, block);
}

// The resolver chain rpcn_client uses (literal, sceNetResolver, getaddrinfo), then getaddrinfo on its own: the
// second line shows whether the musl resolver works by itself.
void diag_resolve(const std::string& host)
{
#ifdef RPCS3_PS4_RPCN_NET
    std::string how;
    const u32 chain_ip = utils::ps4::net::resolve_ipv4(host, how);
    checkpoint("rpcn diag: resolve '%s' -> %s via %s", host.c_str(), chain_ip ? ipv4_text(&chain_ip).c_str() : "none", how.c_str());
#endif

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const int g = ::getaddrinfo(host.c_str(), nullptr, &hints, &result);
    std::string ip = "none";
    if (g == 0 && result && result->ai_addr)
    {
        ip = ipv4_text(&reinterpret_cast<const sockaddr_in*>(result->ai_addr)->sin_addr);
    }
    if (result) ::freeaddrinfo(result);
    checkpoint("rpcn diag: getaddrinfo '%s' ret %d -> %s", host.c_str(), g, ip.c_str());
}

bool job_diag(std::string& message)
{
    checkpoint("rpcn diag: begin, start-up %s", net_prepare().c_str());

    std::string host_line, account_info;
    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        load_config_locked(s.snap);
        host_line = s.snap.host;
        std::string token = g_cfg_rpcn.get_token();
        account_info = fmt::format("NPID '%s', password %s, token %s, %u servers listed", s.snap.npid,
            s.snap.has_password ? "saved" : "empty", token.empty() ? "empty" : "saved", static_cast<u32>(s.snap.hosts.size()));
        acc::wipe(token);
    }
    checkpoint("rpcn diag: rpcn.yml %s, server '%s'", account_info.c_str(), host_line.c_str());
    // Games go online only through the XMB switch file; g_cfg here is whatever the last boot left in it.
    struct stat online_st{};
    const bool online_switch = ::stat(online_flag, &online_st) == 0;
    checkpoint("rpcn diag: online switch %s (%s); g_cfg from the last boot: Internet %s, PSN %s; live RPCS3 threads %u",
        online_switch ? "on" : "off", online_flag, fmt::format("%s", g_cfg.net.net_active.get()).c_str(),
        fmt::format("%s", g_cfg.net.psn_status.get()).c_str(), ps4_thread_registry::get_counters().live);

    diag_net();
    diag_entropy();

    std::string host_name;
    try
    {
        if (const auto parsed = parse_rpcn_host(host_line)) host_name = parsed->first;
    }
    catch (const std::exception&)
    {
        host_name.clear(); // a bad port number throws in parse_rpcn_host
    }
    if (host_name.empty())
        checkpoint("rpcn diag: server '%s' is not a valid host[:port], resolve skipped", host_line.c_str());
    else
        diag_resolve(host_name);

    checkpoint("rpcn diag: end");
    message = i18n::tr(str::RPCNSVC_DIAGNOSTICO_REDE_GRAVADO_DATA_RPCS3);
    return true;
}

void run_job(job type, request& req, bool& ok, std::string& message) noexcept
{
    try
    {
        // libSceNet and libSceNetCtl must be up before the resolver or a socket is used (diag logs the results).
        if (needs_client(type)) net_prepare();

        switch (type)
        {
        case job::test: ok = job_test(message); break;
        case job::login: ok = job_login(req, message); break;
        case job::create: ok = job_create(req, message); break;
        case job::token: ok = job_token(req, message); break;
        case job::resend: ok = job_resend(message); break;
        case job::import_yml: ok = job_import(message); break;
        case job::diag: ok = job_diag(message); break;
        }
    }
    catch (...)
    {
        ok = false;
        // Logging allocates; nothing may leave this noexcept function.
        try
        {
            try
            {
                throw;
            }
            catch (const std::exception& e)
            {
                rpcn_account_log.error("job %s: exception: %s", job_name(type), e.what());
            }
            catch (...)
            {
                rpcn_account_log.error("job %s: unknown exception", job_name(type));
            }
            message = i18n::tr(msg_internal);
        }
        catch (...)
        {
        }
    }

    req.wipe();
}

void account_worker::operator()()
{
    bool ok = false;
    std::string message;
    run_job(type, req, ok, message);
    finish(type, ok, std::move(message));
}
} // namespace

bool start(job type, std::string user, std::string pass, std::string email, std::string token)
{
    request req;
    bool run_inline = false;

    try
    {
        req.user = std::string(acc::trim(user));
        req.pass = std::move(pass);
        acc::wipe(pass); // the move assignment leaves most of a short password in 'pass'
        req.email = std::string(acc::trim(email));
        req.token = acc::normalize_token(token);
        acc::wipe(token);

        auto& s = state();
        std::lock_guard lock(s.mutex);

        if (s.current.busy)
        {
            rpcn_account_log.warning("start(%s) ignored: job already running", job_name(type));
            req.wipe();
            return false;
        }

        if (const std::string_view why = acc::check_request(type, req.user, req.pass, req.email, req.token); !why.empty())
        {
            req.wipe();
            s.current.ok = false;
            s.current.message = std::string(why);
            return false;
        }

        // The previous job thread has returned (busy is false), so this join does not wait.
        delete std::exchange(s.worker, nullptr);

        // Status first: these may throw, and nothing may throw once a worker object holds the request.
        s.current.busy = true;
        s.current.ok = false;
        s.current.message = progress_text(type);
        s.current.npid = s.snap.npid;
        s.started = std::chrono::steady_clock::now();

        const bool pressure = needs_client(type) && utils::ps4::governor::thread_pressure() >= 2;
        worker_thread* worker = nullptr;
        if (!pressure && !refuse_flag_set())
        {
            worker = new (std::nothrow) worker_thread(stx::try_launch, "RPCN Account", type, std::move(req));
        }

        if (worker && worker->ps4_try_start())
        {
            s.worker = worker;
            rpcn_account_log.notice("job %s started", job_name(type));
            return true;
        }

        if (worker)
        {
            // Never started: the object can go at once, without a join.
            worker->req.wipe();
            delete worker;
        }

        const std::string why = pressure ? "thread pressure" : "thread refused";
        checkpoint("RPCN account: job %s not started (%s); live threads: %s", job_name(type), why.c_str(),
            ps4_thread_live_summary().c_str());

        if (type == job::import_yml)
        {
            // Importing is a few file operations: do it here rather than refuse the documented fallback path.
            run_inline = true;
        }
        else
        {
            req.wipe();
            s.current.busy = false;
            s.current.message = i18n::tr(msg_no_resources);
            return false;
        }
    }
    catch (...)
    {
        req.wipe();
        acc::wipe(pass);
        acc::wipe(token);
        try
        {
            try
            {
                throw;
            }
            catch (const std::exception& e)
            {
                rpcn_account_log.error("start(%s): exception: %s", job_name(type), e.what());
            }
            catch (...)
            {
                rpcn_account_log.error("start(%s): unknown exception", job_name(type));
            }

            auto& s = state();
            std::lock_guard lock(s.mutex);
            // A started job keeps its state; anything else is reported and cleared.
            if (!s.worker || !s.current.busy)
            {
                s.current.busy = false;
                s.current.ok = false;
                s.current.message = i18n::tr(msg_internal);
            }
        }
        catch (...)
        {
        }
        return false;
    }

    if (run_inline)
    {
        bool ok = false;
        std::string message;
        run_job(type, req, ok, message);
        finish(type, ok, std::move(message));
    }
    return true;
}

status poll()
{
    try
    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        ensure_snapshot_locked(s);
        status out = s.current;
        out.npid = s.snap.npid;
        if (out.busy)
        {
            // A server that stops answering after the TLS handshake can keep a job waiting for minutes; show that
            // it is still waiting rather than looking frozen.
            const auto waited = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - s.started).count();
            if (waited >= 15) out.message += i18n::trf(str::RPCNSVC_AGUARDANDO_HA, {static_cast<s64>(waited)});
        }
        return out;
    }
    catch (...)
    {
        // Never report "idle" because of an error here: the launcher refuses a game boot while busy.
        status out;
        try
        {
            out.busy = state().current.busy;
        }
        catch (...)
        {
        }
        return out;
    }
}

std::string account_line()
{
    try
    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        ensure_snapshot_locked(s);
        if (s.snap.npid.empty()) return i18n::tr(str::RPCNSVC_NENHUMA_CONTA);
        return i18n::trf(s.snap.has_password ? str::RPCNSVC_ACCOUNT_LINE : str::RPCNSVC_ACCOUNT_LINE_NO_PASSWORD, {s.snap.npid});
    }
    catch (...)
    {
        return i18n::tr(str::RPCNSVC_NENHUMA_CONTA);
    }
}

std::vector<std::string> hosts()
{
    try
    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        ensure_snapshot_locked(s);
        return s.snap.hosts;
    }
    catch (...)
    {
        return {};
    }
}

std::string current_host()
{
    try
    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        ensure_snapshot_locked(s);
        return s.snap.host;
    }
    catch (...)
    {
        return {};
    }
}

void cycle_host()
{
    try
    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        if (s.current.busy) return;

        load_config_locked(s.snap);
        const std::string next = acc::next_host(s.snap.hosts, s.snap.host);
        if (next.empty() || next == s.snap.host) return;

        g_cfg_rpcn.set_host(next);
        if (save_config_locked())
        {
            checkpoint("RPCN account: server set to %s", next.c_str());
            refresh_snapshot_locked(s.snap);
        }
        else
        {
            load_config_locked(s.snap); // back to what is on disk
        }
    }
    catch (const std::exception& e)
    {
        try
        {
            rpcn_account_log.error("cycle_host: exception: %s", e.what());
        }
        catch (...)
        {
        }
    }
    catch (...)
    {
    }
}
} // namespace ps4_rpcn

#endif // RPCS3_PS4
