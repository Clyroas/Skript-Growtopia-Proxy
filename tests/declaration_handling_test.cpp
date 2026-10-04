// Reproduces the exact declaration-handling patterns from
// src/extension/web_server/web_server_impl.hpp so they can be compiled in
// isolation, without the project's Conan/spdlog/httplib dependency tree.
//
// Guards the two bugs found in the first real build:
//   1. assigning to a `const` declaration copy (error C2678)
//   2. locking a non-mutable mutex from a const member function (error C2665)

#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

static int failures = 0;
static void check(bool ok, const std::string& what)
{
    std::cout << (ok ? "ok  : " : "FAIL: ") << what << "\n";
    if (!ok) ++failures;
}

struct ClientDeclaration {
    std::string version{};
    std::string protocol{};
    std::string platform{};
};

static ClientDeclaration parse(const std::string& v, const std::string& p, const std::string& plat = {})
{
    return ClientDeclaration{ v, p, plat };
}

// ---- mirror of WebServerExtension's declaration handling -------------------

class WebServerLike {
public:
    // was: const utils::ClientDeclaration declaration{...}  -> C2678 on assign
    void record_client_declaration(const ClientDeclaration& incoming)
    {
        ClientDeclaration declaration{ incoming };

        if (declaration.version.empty() && declaration.protocol.empty()) {
            return;
        }

        std::lock_guard<std::mutex> lock(declaration_mutex_);

        const bool version_changed{
            !declaration.version.empty() && declaration.version != declared_version_
        };
        const bool protocol_changed{
            !declaration.protocol.empty() && declaration.protocol != declared_protocol_
        };

        if (declaration.version.empty()) {
            declaration.version = declared_version_;
        }
        if (declaration.protocol.empty()) {
            declaration.protocol = declared_protocol_;
        }

        if (!declaration_logged_) {
            last_logged_version_ = declaration.version;
            declaration_logged_ = true;
        }

        declared_version_ = declaration.version;
        declared_protocol_ = declaration.protocol;

        if (version_changed) { saved_version_ = declared_version_; }
        if (protocol_changed) {
            try {
                const int protocol{ std::stoi(declared_protocol_) };
                if (protocol > 0) { saved_protocol_ = static_cast<unsigned int>(protocol); }
            }
            catch (const std::exception&) {
                non_numeric_protocol_ = true;
            }
        }
    }

    // was: const member locking a non-mutable mutex -> C2665
    [[nodiscard]] std::pair<std::string, std::string> client_declaration() const
    {
        std::lock_guard<std::mutex> lock(declaration_mutex_);
        return { declared_version_, declared_protocol_ };
    }

    [[nodiscard]] const std::string& saved_version() const { return saved_version_; }
    [[nodiscard]] unsigned int saved_protocol() const { return saved_protocol_; }
    [[nodiscard]] bool non_numeric() const { return non_numeric_protocol_; }
    [[nodiscard]] const std::string& logged() const { return last_logged_version_; }

private:
    std::string declared_version_;
    std::string declared_protocol_;
    bool declaration_logged_{ false };
    // the fix
    mutable std::mutex declaration_mutex_;

    std::string saved_version_{};
    unsigned int saved_protocol_{ 0 };
    bool non_numeric_protocol_{ false };
    std::string last_logged_version_{};
};

int main()
{
    std::cout << "=== compiling at all proves the two fixes ===\n";
    check(true, "const-copy assignment and mutable-mutex const lock both compile");

    std::cout << "\n=== behaviour is preserved ===\n";
    {
        WebServerLike w{};
        w.record_client_declaration(parse("5.36", "245", "0"));
        const auto [v, p] = w.client_declaration();
        check(v == "5.36", "version recorded");
        check(p == "245", "protocol recorded");
        check(w.saved_version() == "5.36", "version persisted");
        check(w.saved_protocol() == 245u, "protocol persisted as unsigned");
        check(w.logged() == "5.36", "first declaration logged once");
    }

    std::cout << "\n=== partial declarations fill in from remembered values ===\n";
    {
        WebServerLike w{};
        w.record_client_declaration(parse("5.36", "245"));
        w.record_client_declaration(parse("", "245"));   // version missing
        const auto [v, p] = w.client_declaration();
        check(v == "5.36", "missing version backfilled from previous");
        check(p == "245", "protocol unchanged");
    }
    {
        WebServerLike w{};
        w.record_client_declaration(parse("5.36", "245"));
        w.record_client_declaration(parse("5.37", ""));  // protocol missing
        const auto [v, p] = w.client_declaration();
        check(v == "5.37", "new version adopted");
        check(p == "245", "missing protocol backfilled from previous");
    }

    std::cout << "\n=== empty declaration is ignored ===\n";
    {
        WebServerLike w{};
        w.record_client_declaration(parse("", ""));
        const auto [v, p] = w.client_declaration();
        check(v.empty() && p.empty(), "nothing recorded from an empty body");
    }

    std::cout << "\n=== non-numeric protocol is survivable ===\n";
    {
        WebServerLike w{};
        w.record_client_declaration(parse("5.36", "abc"));
        const auto [v, p] = w.client_declaration();
        check(v == "5.36", "version still recorded");
        check(p == "abc", "raw protocol text kept for logging");
        check(w.non_numeric(), "non-numeric protocol noted");
        check(w.saved_protocol() == 0u, "no bogus protocol persisted");
    }

    std::cout << "\n=== concurrent reads are safe (const accessor + lock) ===\n";
    {
        WebServerLike w{};
        w.record_client_declaration(parse("5.36", "245"));
        std::string seen;
        std::thread reader([&] { for (int i = 0; i < 2000; ++i) { auto d = w.client_declaration(); seen = d.first; } });
        std::thread writer([&] { for (int i = 0; i < 2000; ++i) { w.record_client_declaration(parse("5.36", "245")); } });
        reader.join();
        writer.join();
        check(seen == "5.36", "const accessor readable while writer runs");
    }

    std::cout << "\n=== summary ===\n";
    if (failures == 0) { std::cout << "ALL CHECKS PASSED\n"; return 0; }
    std::cout << failures << " CHECK(S) FAILED\n";
    return 1;
}
