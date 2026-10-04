#include <cassert>
#include <iostream>
#include <string>

#include "../src/utils/server_data_parser.hpp"
#include "../src/utils/client_info.hpp"

static int failures = 0;

static void check(bool condition, const std::string& what)
{
    if (!condition) {
        std::cout << "FAIL: " << what << "\n";
        ++failures;
    }
    else {
        std::cout << "ok  : " << what << "\n";
    }
}

static void check_eq(const std::string& actual, const std::string& expected, const std::string& what)
{
    if (actual != expected) {
        std::cout << "FAIL: " << what << "\n  expected: [" << expected << "]\n  actual  : [" << actual << "]\n";
        ++failures;
    }
    else {
        std::cout << "ok  : " << what << "\n";
    }
}

int main()
{
    std::cout << "=== ServerDataParser ===\n";

    const std::string real =
        "\n"
        "server|5.21.0.1\n"
        "port|17091\n"
        "type|1\n"
        "#maint|Maintenance message\n"
        "beta_server|5.21.0.2\n"
        "beta_port|17092\n"
        "beta_type|1\n"
        "meta|localhost\n"
        "RTENDMARKERBS1001\n";

    utils::ServerDataParser parser{ real };

    check(parser.has_field("server"), "server field detected");
    check_eq(parser.get("server"), "5.21.0.1", "upstream server host parsed");
    check(parser.has_field("beta_server"), "beta_server field detected");
    check(parser.has_field("meta"), "meta field detected");

    const auto port = utils::parse_port(parser.get("port"));
    check(port.has_value() && *port == 17091, "port parsed as 17091");

    check(parser.set("server", "127.0.0.1"), "server overridden");
    check(parser.set("port", "17091"), "port overridden");
    check(!parser.set("nonexistent", "x"), "setting absent field reports false");
    check(parser.set("beta_server", "127.0.0.1"), "beta_server overridden");
    check(parser.set("beta_port", "17091"), "beta_port overridden");

    const std::string rewritten = parser.serialize();

    check(rewritten.find("server|127.0.0.1\n") != std::string::npos, "server rewritten");
    check(rewritten.find("port|17091\n") != std::string::npos, "port rewritten");
    check(rewritten.find("RTENDMARKERBS1001\n") != std::string::npos, "end marker preserved");
    check(rewritten.find("meta|localhost\n") != std::string::npos, "meta preserved");
    check(rewritten.find("#maint|Maintenance message\n") != std::string::npos, "maint line preserved");
    check(rewritten.find("beta_port|17091\n") != std::string::npos, "beta_port rewritten");
    check(rewritten.find("beta_server|127.0.0.1\n") != std::string::npos, "beta_server rewritten");

    check(rewritten.find("server|127.0.0.1\nport|17091\n") != std::string::npos,
          "existing field order preserved");

    std::cout << "\n--- rewritten payload (dots = newline) ---\n";
    for (const char ch : rewritten) {
        std::cout << (ch == '\n' ? '.' : ch);
    }
    std::cout << "\n";

    std::cout << "\n=== CRLF tolerance ===\n";
    utils::ServerDataParser crlf{ "server|1.2.3.4\r\nport|1234\r\nmeta|x\r\n" };
    check_eq(crlf.get("server"), "1.2.3.4", "CRLF stripped from value");
    check(crlf.serialize().find("server|1.2.3.4\n") != std::string::npos, "CRLF normalised on output");

    std::cout << "\n=== error / non-field payloads ===\n";
    utils::ServerDataParser error{ "Please update your client to continue.\n" };
    check(!error.has_field("server"), "error payload has no server field");
    check(!utils::parse_port(error.get("port")).has_value(), "missing port yields nullopt");

    utils::ServerDataParser html{ "<html><body>403 Forbidden</body></html>" };
    check(!html.has_field("server"), "html payload has no server field");

    std::cout << "\n=== port parsing edge cases ===\n";
    check(!utils::parse_port("").has_value(), "empty port rejected");
    check(!utils::parse_port("abc").has_value(), "non-numeric port rejected");
    check(!utils::parse_port("0").has_value(), "port 0 rejected");
    check(!utils::parse_port("70000").has_value(), "out-of-range port rejected");
    check(utils::parse_port("65535").has_value(), "port 65535 accepted");
    check(utils::parse_port("443abc").has_value(), "leading-numeric port accepted (from_chars)");

    std::cout << "\n=== form field helpers ===\n";
    const std::string body = "version=5.36&platform=0&protocol=245";
    const utils::ClientDeclaration decl = utils::parse_client_declaration(body);
    check_eq(decl.version, "5.36", "version extracted");
    check_eq(decl.protocol, "245", "protocol extracted");
    check_eq(decl.platform, "0", "platform extracted");

    const auto missing = utils::find_form_field(body, "absent");
    check(!missing.has_value(), "absent field yields nullopt");

    const std::string replaced = utils::set_form_field(body, "version", "9.99");
    check_eq(replaced, "version=9.99&platform=0&protocol=245", "existing field replaced in place");

    const std::string appended = utils::set_form_field(body, "newkey", "a b");
    check_eq(appended, "version=5.36&platform=0&protocol=245&newkey=a%20b", "new field appended and encoded");

    const utils::ClientDeclaration encoded =
        utils::parse_client_declaration("version=5.36%2E1&protocol=245");
    check_eq(encoded.version, "5.36.1", "percent-decoding works");

    const utils::ClientDeclaration empty = utils::parse_client_declaration("");
    check(empty.version.empty() && empty.protocol.empty(), "empty body yields empty declaration");

    std::cout << "\n=== summary ===\n";
    if (failures == 0) {
        std::cout << "ALL CHECKS PASSED\n";
        return 0;
    }
    std::cout << failures << " CHECK(S) FAILED\n";
    return 1;
}
