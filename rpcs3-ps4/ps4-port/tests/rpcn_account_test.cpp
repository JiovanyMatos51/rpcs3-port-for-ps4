// Host test for the RPCN account helpers (rpcs3/util/ps4_rpcn_account.hpp) and the on-screen keyboard text
// conversion (rpcs3/ps4_ime.h).
//
// derive_password must give the same 64 hex digits as the PC account dialog (rpcs3qt/rpcn_settings_dialog.cpp),
// otherwise an account made on one side cannot sign in on the other. It runs here on the same 3rdparty/wolfssl
// sources the console build compiles, and the expected values below come from an independent implementation
// (Python hashlib.pbkdf2_hmac('sha3_256', password, salt, 200000, 32), cross-checked with a plain HMAC loop).
//
// Optional PC parity check (never share rpcn.yml): in a PC RPCS3, sign in to a test account whose
// typed password is "rpcs3-ps4 test"; the Password line of the PC's config/rpcn.yml must then read the first
// vector below. Any password works the same way: the console stores exactly what the PC stores.
//
// Build and run from any scratch directory (llvm-mingw, already used for the other host tests):
//   W=3rdparty/wolfssl/wolfssl
//   clang -c -O2 -DWOLFSSL_USER_SETTINGS -I ps4-port/tests/rpcn-account-mocks -I $W $W/wolfcrypt/src/pwdbased.c
//     $W/wolfcrypt/src/hmac.c $W/wolfcrypt/src/hash.c $W/wolfcrypt/src/sha3.c $W/wolfcrypt/src/sha256.c
//     $W/wolfcrypt/src/memory.c $W/wolfcrypt/src/wc_port.c $W/wolfcrypt/src/logging.c $W/wolfcrypt/src/error.c
//   clang++ -std=c++20 -static -Wall -Wextra -DWOLFSSL_USER_SETTINGS -I ps4-port/tests/rpcn-account-mocks -I $W
//     -I rpcs3 ps4-port/tests/rpcn_account_test.cpp *.o -o rpcn_account_test.exe
//   rpcn_account_test.exe

#include "util/ps4_rpcn_account.hpp"
#include "ps4_ime.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
int g_failures = 0;

#define CHECK(expr) \
    do \
    { \
        if (!(expr)) \
        { \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            g_failures++; \
        } \
    } while (0)

namespace acc = utils::ps4::rpcn_account;
using ps4_rpcn::job;

void test_derive_password()
{
    struct vector
    {
        std::string password;
        const char* expected;
    };

    const vector vectors[] = {
        {"rpcs3-ps4 test", "A68D74C6B11B869C95A7A9842ADD973600328195F495C983D408DB263F2CFD2E"},
        {"Senha \xC3\xA7\xC3\xA3 123", "555886E33E4AE6D316120AD28E2CF71499717482BC55CCE0F6C348E00EB96476"}, // UTF-8, as the keyboard returns it
        {"a", "CEB23E5E7DBAA33EAE187232266A8D6BCC55D768222EDBBC7775E8CE9BEB4789"},
        {std::string(200, 'x'), "34D451018916ADDFCE904C57C50BF05BE7C9C608B3634101BAE9311DEDB1715A"}, // HMAC key longer than a block
    };

    for (const auto& v : vectors)
    {
        const std::string derived = acc::derive_password(v.password);
        CHECK(derived == v.expected);
        CHECK(acc::is_derived_password(derived));
        if (derived != v.expected) std::printf("  got %s\n  want %s\n", derived.c_str(), v.expected);
    }
}

void test_validation()
{
    CHECK(acc::validate_username("abc"));
    CHECK(acc::validate_username("Player_97-PS4"));
    CHECK(acc::validate_username("1234567890123456"));
    CHECK(!acc::validate_username("ab"));
    CHECK(!acc::validate_username("12345678901234567"));
    CHECK(!acc::validate_username("bad name"));
    CHECK(!acc::validate_username("jo\xC3\xA3o"));
    CHECK(!acc::validate_username("a.b"));

    CHECK(acc::validate_token("0123456789ABCDEZ"));
    CHECK(!acc::validate_token("0123456789abcdef"));
    CHECK(!acc::validate_token("0123456789ABCDE"));
    CHECK(!acc::validate_token("0123456789ABCDEF0"));
    CHECK(acc::normalize_token("  0123456789abcdez\n") == "0123456789ABCDEZ");
    CHECK(acc::validate_token(acc::normalize_token("0123456789abcdez")));

    CHECK(acc::validate_email("user@example.com"));
    CHECK(acc::validate_email("first.last+tag@mail.example.com.br"));
    CHECK(!acc::validate_email(""));
    CHECK(!acc::validate_email("user@localhost"));
    CHECK(!acc::validate_email("user@@example.com"));
    CHECK(!acc::validate_email("@example.com"));
    CHECK(!acc::validate_email("user@example..com"));
    CHECK(!acc::validate_email("user@.example.com"));
    CHECK(!acc::validate_email("user@example.com."));
    CHECK(!acc::validate_email("us er@example.com"));
    CHECK(!acc::validate_email("user@exa_mple.com"));

    CHECK(acc::is_derived_password(std::string(64, 'A')));
    CHECK(acc::is_derived_password(std::string(64, 'f')));
    CHECK(!acc::is_derived_password(std::string(63, 'A')));
    CHECK(!acc::is_derived_password(std::string(64, 'G')));
    CHECK(!acc::is_derived_password("plain password"));

    CHECK(acc::trim("  a b \r\n") == "a b");
    CHECK(acc::trim(" \t ").empty());
}

void test_requests()
{
    CHECK(acc::check_request(job::test, "", "", "", "").empty());
    CHECK(acc::check_request(job::resend, "", "", "", "").empty());
    CHECK(acc::check_request(job::import_yml, "", "", "", "").empty());
    CHECK(acc::check_request(job::diag, "", "", "", "").empty());

    CHECK(acc::check_request(job::login, "user1", "secret", "", "").empty());
    CHECK(acc::check_request(job::login, "user1", "secret", "", "0123456789ABCDEF").empty());
    CHECK(!acc::check_request(job::login, "user1", "secret", "", "short").empty());
    CHECK(!acc::check_request(job::login, "u", "secret", "", "").empty());
    CHECK(!acc::check_request(job::login, "user1", "", "", "").empty());
    CHECK(!acc::check_request(job::login, "user1", std::string(acc::password_max + 1, 'p'), "", "").empty());

    CHECK(acc::check_request(job::create, "user1", "secret", "user@example.com", "").empty());
    CHECK(!acc::check_request(job::create, "user1", "secret", "", "").empty());
    CHECK(!acc::check_request(job::create, "user 1", "secret", "user@example.com", "").empty());

    CHECK(acc::check_request(job::token, "", "", "", "0123456789ABCDEF").empty());
    CHECK(!acc::check_request(job::token, "", "", "", "").empty());

    // The password never shows up in the text returned for the XMB.
    const std::string_view why = acc::check_request(job::login, "x", "hunter2-secret", "", "");
    CHECK(!why.empty() && why.find("hunter2") == std::string_view::npos);
}

void test_import_check()
{
    const std::string good(64, 'A');
    CHECK(acc::check_import("user1", good, 2).empty());
    CHECK(acc::check_import("user1", good, 3).empty());
    CHECK(!acc::check_import("user1", good, 1).empty());
    CHECK(!acc::check_import("", good, 2).empty());
    CHECK(!acc::check_import("user1", "", 2).empty());
    CHECK(!acc::check_import("user1", "typed password", 2).empty());

    // The texts follow the interface language
    namespace i18n = utils::ps4::i18n;
    CHECK(acc::check_import("user1", good, 1) == "rpcn.yml antigo: entre na conta no RPCS3 do PC e copie de novo");
    i18n::apply(i18n::setting::en, -1);
    CHECK(acc::check_import("user1", good, 1) == "old rpcn.yml: sign in on the PC's RPCS3 and copy it again");
    CHECK(acc::check_request(job::login, "user1", "", "", "") == "Enter the password");
    i18n::apply(i18n::setting::es, -1);
    CHECK(acc::check_request(job::create, "user1", "secret", "", "") == "Correo electrónico no válido");
    i18n::apply(i18n::setting::automatic, -1);
    CHECK(acc::check_request(job::login, "user1", "", "", "") == "Digite a senha");
}

void test_hosts()
{
    const auto one = acc::parse_hosts("Official RPCN Server|np.rpcs3.net");
    CHECK(one.size() == 1 && one[0].description == "Official RPCN Server" && one[0].address == "np.rpcs3.net");

    const auto two = acc::parse_hosts("Official RPCN Server|np.rpcs3.net|||Local|192.168.0.10:31313|||broken|||a|b|c");
    CHECK(two.size() == 2);
    CHECK(two.size() == 2 && two[1].address == "192.168.0.10:31313");
    CHECK(acc::parse_hosts("").empty());

    const std::vector<std::string> list = {"np.rpcs3.net", "192.168.0.10:31313"};
    CHECK(acc::next_host(list, "np.rpcs3.net") == "192.168.0.10:31313");
    CHECK(acc::next_host(list, "192.168.0.10:31313") == "np.rpcs3.net");
    CHECK(acc::next_host(list, "other") == "np.rpcs3.net");
    CHECK(acc::next_host({}, "np.rpcs3.net") == "np.rpcs3.net");
}

void test_wipe()
{
    std::string secret = "secret";
    acc::wipe(secret);
    CHECK(secret.empty());

    // A move-assigned-from short string keeps its old characters after the first one; wipe must clear them too.
    std::string typed = "password123";
    std::string target;
    target = std::move(typed);
    acc::wipe(typed);
    bool clean = typed.empty();
    const char* raw = typed.data();
    for (std::size_t i = 0; i < typed.capacity(); i++)
        if (raw[i] != 0) clean = false;
    CHECK(clean);
    CHECK(target == "password123");
}

void test_utf()
{
    using ps4_ime::utf::to_utf16;
    using ps4_ime::utf::to_utf8;

    char16_t buf[32];
    const std::string samples[] = {
        "abc",
        "Senha \xC3\xA7\xC3\xA3 123",       // 2-byte sequences
        "\xE2\x82\xAC 5",                   // euro sign, 3 bytes
        "\xF0\x9F\x8E\xAE game",            // U+1F3AE, surrogate pair
    };
    for (const auto& s : samples)
    {
        const std::size_t n = to_utf16(s, buf, 32);
        std::string back;
        to_utf8(buf, n, back);
        CHECK(back == s);
    }

    CHECK(to_utf16("\xF0\x9F\x8E\xAE", buf, 32) == 2 && buf[0] == 0xD83C && buf[1] == 0xDFAE);

    // Invalid input becomes U+FFFD, never a crash or a partial sequence.
    CHECK(to_utf16("a\xFF" "b", buf, 32) == 3 && buf[1] == 0xFFFD && buf[2] == u'b');
    CHECK(to_utf16("\xC3", buf, 32) == 1 && buf[0] == 0xFFFD);
    CHECK(to_utf16("\xED\xA0\x80", buf, 32) >= 1 && buf[0] == 0xFFFD); // encoded surrogate
    CHECK(to_utf16("\xC0\xAF", buf, 32) >= 1 && buf[0] == 0xFFFD);     // overlong '/'

    // Truncation keeps the terminator and never splits a surrogate pair.
    CHECK(to_utf16("abcdef", buf, 4) == 3 && buf[3] == 0);
    CHECK(to_utf16("a\xF0\x9F\x8E\xAE", buf, 3) == 1 && buf[1] == 0);
    CHECK(to_utf16("abc", buf, 1) == 0 && buf[0] == 0);
    CHECK(to_utf16("abc", buf, 0) == 0);

    // Lone surrogates from the system become U+FFFD; the length limit and a 0 unit stop the conversion.
    const char16_t lone[] = {u'a', 0xD800, u'b', 0xDC00, 0};
    std::string out;
    to_utf8(lone, 5, out);
    CHECK(out == "a\xEF\xBF\xBD" "b\xEF\xBF\xBD");
    out.clear();
    to_utf8(lone, 1, out);
    CHECK(out == "a");
}
} // namespace

int main()
{
    test_derive_password();
    test_validation();
    test_requests();
    test_import_check();
    test_hosts();
    test_wipe();
    test_utf();

    if (g_failures)
    {
        std::printf("rpcn_account_test: %d FAILED\n", g_failures);
        return 1;
    }
    std::printf("rpcn_account_test: all passed\n");
    return 0;
}
