#pragma once

// 0.31: RPCN account service for the XMB launcher. The implementation (ps4_rpcn_service.cpp) is built in the NP
// target, which has exceptions enabled and the wolfSSL defines; this header includes neither wolfSSL nor
// rpcn_client.h, so frontend files built without exceptions can call it. Jobs run one at a time on an
// "RPCN Account" worker thread, so the XMB keeps drawing while a job connects. No exception leaves these functions.

#include <string>
#include <vector>

namespace ps4_rpcn
{
enum class job
{
    test,       // connect and log in with the saved account
    login,      // save user name and password (derived the same way as the PC dialog), then test
    create,     // create an account; the server emails a token. Start it only after the player confirmed the
                // filled-in form: it makes a permanent account on the server, and the service never retries it
    token,      // save the email token, then test
    resend,     // ask the server to email the token again
    import_yml, // import rpcn.yml from /mnt/usb0|usb1/rpcs3/, /mnt/usb0|usb1/ or /data/rpcs3/import/
    diag,       // network diagnostics written to startup.log
};

struct status
{
    bool busy = false, ok = false; // busy: a job is running; ok: result of the last finished job
    std::string message, npid;     // message: progress or result text for the XMB; npid: account name, empty if none
};

// Starts a job. Returns false when a job is already running (the running job's status is kept), when the input
// is incomplete or invalid, or when the worker thread could not be created; in the last two cases the status
// message says why ("Sem recursos para conectar agora" for the thread). import_yml is a few file operations and
// runs on the calling thread instead when its thread is refused. Inputs needed: login user+pass (+ optional token),
// create user+pass+email, token token; the others take none. User, email and token are trimmed, and the token is
// upper-cased.
// 'pass' is the password as typed (callers std::move it in); it is never logged, never put in a status message
// and never stored as typed, and the service wipes its copy after deriving the key. 'token' (and the saved
// Password/Token of rpcn.yml) are never logged either.
bool start(job type, std::string user = {}, std::string pass = {}, std::string email = {}, std::string token = {});

// Current state; cheap enough to call every frame. The first call of poll/account_line/hosts/current_host reads
// rpcn.yml once. After 15 s of a running job, 'message' ends with " (aguardando há N s)". If the state cannot be
// read, busy still reports whether a job runs.
status poll();

// One XMB line describing the saved account: "Conta: <npid>", "Conta: <npid> (sem senha)" or "Nenhuma conta".
std::string account_line();

// Server addresses of the rpcn.yml "Hosts" list, in order (the server in use is the "Host" value).
std::vector<std::string> hosts();

// The rpcn.yml "Host" value, i.e. the server the next job connects to.
std::string current_host();

// Sets "Host" to the entry after the current one in hosts() (wrapping around) and saves rpcn.yml.
// Ignored while a job is running.
void cycle_host();
} // namespace ps4_rpcn
