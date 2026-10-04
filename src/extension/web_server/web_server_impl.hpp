#pragma once
#include <httplib.h>
#include <magic_enum/magic_enum.hpp>
#include <nlohmann/json.hpp>
#include <regex>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <utility>

#include "web_server.hpp"
#include "../../utils/client_info.hpp"
#include "../../utils/server_data_parser.hpp"
#include "../../utils/strenc.hpp"
#include "../../client/client.hpp"
#include "../../core/core.hpp"
#include "../../utils/network.hpp"

namespace extension::web_server {
class WebServerExtension final : public IWebServerExtension {
    core::Core* core_;
    httplib::SSLServer server_;

    std::string address_;
    uint16_t port_;

    std::string declared_version_;
    std::string declared_protocol_;
    bool declaration_logged_{ false };
    // mutable: client_declaration() is const but still needs to take the lock
    mutable std::mutex declaration_mutex_;

public:
    explicit WebServerExtension(core::Core* core)
        : core_{ core }
        , server_{ "./resources/cert.pem", "./resources/key.pem" }
        , port_{ 65535 }
    {
    }

    ~WebServerExtension() override
    {
        server_.stop();
    }

    void init() override
    {
        core_->get_event_dispatcher().prependListener(
            core::EventType::Connection,
            [&](const core::EventConnection& evt)
            {
                if (evt.from != core::EventFrom::FromClient) {
                    return;
                }

                
                if (evt.get_player().get_peer()->address.host != 16777343) {
                    spdlog::info("Security alert: External connection attempt blocked");
                    evt.canceled = true;
                    return;
                }

                if (address_.empty() || port_ == 65535) {
                    return;
                }

                std::ignore = core_->get_client()->connect(address_, port_);
                evt.canceled = true;

                address_.clear();
                port_ = 65535;
            }
        );

        
        check_ca_certificate();

        
        server_.set_logger([](const httplib::Request& req, const httplib::Response& res)
        {
            std::string method_color = "\033[1;36m"; 
            std::string status_color = res.status >= 400 ? "\033[1;31m" : "\033[1;32m"; 
            
            spdlog::info("HTTP {} {}{}\033[0m -> {}{}\033[0m", 
                method_color, req.method, req.path,
                status_color, res.status);
        });

        server_.set_error_handler([](const httplib::Request&, httplib::Response& res)
        {
            nlohmann::json error_response = {
                {"status", "error"},
                {"code", res.status},
                {"message", httplib::status_message(res.status)},
                {"timestamp", std::chrono::system_clock::now().time_since_epoch().count()}
            };
            
            res.set_content(error_response.dump(), "application/json");
        });

        server_.set_exception_handler([](const httplib::Request&, httplib::Response& res, const std::exception_ptr& ep)
        {
            res.status = 500;
            
            nlohmann::json error_response = {
                {"status", "error"},
                {"code", 500},
                {"message", "Internal server error"},
                {"timestamp", std::chrono::system_clock::now().time_since_epoch().count()}
            };

            try {
                std::rethrow_exception(ep);
            }
            catch (std::exception &e) {
                error_response["details"] = e.what();
            }
            catch (...) {
                error_response["details"] = "Unknown exception occurred";
            }

            res.set_content(error_response.dump(), "application/json");
        });

        if (!server_.bind_to_port("127.0.0.1", 443)) {
            spdlog::info("HTTPS server failed to bind to port 443");
            return;
        }

        spdlog::trace("HTTPS server initialized on port 443");
        std::thread{ &WebServerExtension::listen_internal, this }.detach();
    }

    void free() override
    {
        delete this;
    }

private:
    void check_ca_certificate() {
        std::string ca_cert_path = "./resources/cacert.pem";
        if (std::filesystem::exists(ca_cert_path)) {
            std::ifstream file(ca_cert_path);
            std::string content((std::istreambuf_iterator<char>(file)),
                                 std::istreambuf_iterator<char>());
            if (content.size() > 100) {
                spdlog::trace("CA certificate found ({} bytes): {}", content.size(), ca_cert_path);
            } else {
                spdlog::warn("CA certificate file is too small ({} bytes). Download fresh copy.", content.size());
            }
        } else {
            spdlog::warn("CA certificate not found: {}", ca_cert_path);
        }
    }

    bool validate_server_response(const httplib::Result& response)
    {
        if (!response) {
            spdlog::info("Network error: {}", httplib::to_string(response.error()));
            return false;
        }

        if (response->status != 200) {
            spdlog::info("Server responded with status: {}", response->status);
            return false;
        }

        return true;
    }

    
    std::unique_ptr<httplib::SSLClient> create_ssl_client(
        const std::string& host,
        const std::string& resolved_address = {}) {
        auto cli = std::make_unique<httplib::SSLClient>(host);
        if (!resolved_address.empty()) {
            // Connect to the DoH-resolved IP while keeping the original hostname for
            // SNI and certificate hostname verification. Constructing SSLClient with
            // the IP makes valid domain certificates fail verification.
            cli->set_hostname_addr_map({ { host, resolved_address } });
        }
        cli->set_connection_timeout(10);
        cli->set_read_timeout(30);
        cli->set_write_timeout(10);
        
        
        std::string ca_cert_path = "./resources/cacert.pem";
        if (std::filesystem::exists(ca_cert_path)) {
            cli->set_ca_cert_path(ca_cert_path);
            spdlog::debug("Using CA certificate: {}", ca_cert_path);
        }
        
        
        cli->enable_server_certificate_verification(true);
        
        return cli;
    }

    
    
    void record_client_declaration(const httplib::Request& req)
    {
        utils::ClientDeclaration declaration{
            utils::parse_client_declaration(req.body)
        };

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
            spdlog::info("Growtopia client declared version={} protocol={} platform={}",
                declaration.version.empty() ? "?" : declaration.version,
                declaration.protocol.empty() ? "?" : declaration.protocol,
                declaration.platform.empty() ? "?" : declaration.platform);
            declaration_logged_ = true;
        }

        declared_version_ = declaration.version;
        declared_protocol_ = declaration.protocol;

        if (version_changed) {
            core_->get_config().set<std::string>("client.game_version", declared_version_);
        }
        if (protocol_changed) {
            try {
                const int protocol{ std::stoi(declared_protocol_) };
                if (protocol > 0) {
                    core_->get_config().set<unsigned int>(
                        "client.protocol", static_cast<unsigned int>(protocol));
                }
            }
            catch (const std::exception&) {
                spdlog::warn("Client declared non-numeric protocol \"{}\"", declared_protocol_);
            }
        }
    }

    
    
    void report_server_data_failure(httplib::Response& res,
                                    const std::string& response_body,
                                    const std::string& target_server)
    {
        const std::string preview{ make_preview(response_body) };

        const auto [version, protocol] = client_declaration();

        spdlog::error("Growtopia rejected the login request (client version={} protocol={})",
            version.empty() ? "unknown" : version,
            protocol.empty() ? "unknown" : protocol);
        spdlog::error("Upstream {} replied: {}", target_server,
            preview.empty() ? "<empty body>" : preview);

        res.status = 502;
        res.set_content(
            "Growtopia did not return a usable server_data.php response.\n"
            "Client version : " + (version.empty() ? std::string{ "unknown" } : version) + "\n"
            "Client protocol: " + (protocol.empty() ? std::string{ "unknown" } : protocol) + "\n"
            "Upstream reply : " + (preview.empty() ? std::string{ "<empty>" } : preview) + "\n",
            "text/plain"
        );
    }

    [[nodiscard]] std::pair<std::string, std::string> client_declaration() const
    {
        std::lock_guard<std::mutex> lock(declaration_mutex_);
        return { declared_version_, declared_protocol_ };
    }

    static std::string make_preview(const std::string& body)
    {
        constexpr std::size_t max_preview{ 400 };

        std::string preview{};
        preview.reserve(std::min(body.size(), max_preview));

        for (const char ch : body) {
            if (preview.size() >= max_preview) {
                preview += "...";
                break;
            }

            if (ch == '\n' || ch == '\r' || ch == '\t') {
                preview += ' ';
            }
            else if (static_cast<unsigned char>(ch) >= 0x20) {
                preview += ch;
            }
        }

        return preview;
    }

    std::string resolve_domain_name(const std::string& domain_name)
    {
        std::string host = core_->get_config().get("client.dnsServer") == "cloudflare" 
            ? "cloudflare-dns.com" 
            : "dns.google";
        
        std::string path = core_->get_config().get("client.dnsServer") == "cloudflare" 
            ? "/dns-query" 
            : "/resolve";

        httplib::Headers headers = {
            { "Accept", "application/dns-json" },
            { "User-Agent", "SkriptProxy/2.0" }
        };

        auto cli = create_ssl_client(host);
        
        auto res = cli->Get(path + "?name=" + domain_name + "&type=A", headers);
        
        if (!validate_server_response(res)) {
            return "";
        }

        try {
            auto j = nlohmann::json::parse(res->body);
            if (j["Status"] != 0) {
                return "";
            }
            
            return j["Answer"].back()["data"].get<std::string>();
        }
        catch (...) {
            return "";
        }
    }

    
    [[nodiscard]] std::string build_upstream_body(const std::string& client_body) const
    {
        const std::string version_override{
            core_->get_config().get<std::string>("client.version_override", "")
        };

        const unsigned int protocol_override{
            core_->get_config().get<unsigned int>("client.protocol_override", 0u)
        };

        if (version_override.empty() && protocol_override == 0) {
            return client_body;
        }

        std::string body{ client_body };

        if (!version_override.empty()) {
            body = utils::set_form_field(body, "version", version_override);
        }
        if (protocol_override != 0) {
            body = utils::set_form_field(body, "protocol", std::to_string(protocol_override));
        }

        spdlog::warn("Overriding client declaration sent upstream (version={} protocol={})",
            version_override.empty() ? "<client>" : version_override,
            protocol_override == 0 ? "<client>" : std::to_string(protocol_override));

        return body;
    }

    void listen_internal()
    {
        
        server_.Post("/growtopia/server_data.php", [&](
            const httplib::Request& req,
            httplib::Response& res
        ) {
            
            res.set_header("X-Content-Type-Options", "nosniff");
            res.set_header("X-Frame-Options", "DENY");
            res.set_header("X-XSS-Protection", "1; mode=block");
            res.set_header("Strict-Transport-Security", "max-age=31536000");

            const std::string target_hostname{ core_->get_config().get("server.address") };
            const std::string target_server{ resolve_domain_name(target_hostname) };

            if (target_server.empty()) {
                res.status = 502;
                res.set_content("Failed to resolve server address", "text/plain");
                return true;
            }

            spdlog::info("Connecting to Growtopia server: {}", target_server);

            record_client_declaration(req);

            
            const std::string upstream_body{ build_upstream_body(req.body) };

            bool connection_success = false;
            std::string response_body;
            
            
            {
                auto cli = create_ssl_client(target_hostname, target_server);
                auto result = cli->Post("/growtopia/server_data.php", 
                    {{ "User-Agent", req.get_header_value("User-Agent") },
                     { "Host", core_->get_config().get("server.address") },
                     { "Content-Type", "application/x-www-form-urlencoded" }},
                    upstream_body, "application/x-www-form-urlencoded");
                
                if (validate_server_response(result)) {
                    connection_success = true;
                    response_body = result->body;
                    spdlog::info("Successfully connected using SSL verification");
                } else {
                    spdlog::warn("Verified upstream HTTPS request failed; refusing an unverified retry");
                }
            }
            
            if (!connection_success) {
                res.status = 502;
                res.set_content("Cannot connect to Growtopia server", "text/plain");
                spdlog::error("All connection attempts failed to {}", target_server);
                return true;
            }

            try {
                
                
                utils::ServerDataParser server_data{ response_body };

                
                
                if (!server_data.has_field("server") || server_data.get("server").empty()) {
                    report_server_data_failure(res, response_body, target_server);
                    return true;
                }

                const std::optional<unsigned int> upstream_port{
                    utils::parse_port(server_data.get("port"))
                };

                if (!upstream_port) {
                    report_server_data_failure(res, response_body, target_server);
                    return true;
                }

                address_ = server_data.get("server");
                port_ = static_cast<uint16_t>(*upstream_port);

                
                
                
                
                
                
                
                server_data.set("server", "127.0.0.1");
                server_data.set("port", std::to_string(
                    core_->get_config().get<unsigned int>("server.port", 17091)
                ));

                
                if (server_data.has_field("beta_server")) {
                    server_data.set("beta_server", "127.0.0.1");
                }
                if (server_data.has_field("beta_port")) {
                    server_data.set("beta_port", std::to_string(
                        core_->get_config().get<unsigned int>("server.port", 17091)
                    ));
                }
                if (server_data.has_field("type2")) {
                    server_data.set("type2", "1");
                }

                res.set_content(server_data.serialize(), "text/html");

                const auto [version, protocol] = client_declaration();
                spdlog::info("Proxied server_data.php -> {}:{} (client version {}, protocol {})",
                    address_,
                    port_,
                    version.empty() ? "unknown" : version,
                    protocol.empty() ? "unknown" : protocol);
                return true;
            }
            catch (const std::exception& e) {
                spdlog::info("Error processing server response: {}", e.what());
                res.status = 500;
                res.set_content("Internal server error", "text/plain");
                return true;
            }
            catch (...) {
                spdlog::info("Unknown error processing server response");
                res.status = 500;
                res.set_content("Internal server error", "text/plain");
                return true;
            }
        });

        
        server_.Get("/health", [](const httplib::Request&, httplib::Response& res) {
            nlohmann::json health_status = {
                {"status", "healthy"},
                {"service", "skript-proxy"},
                {"timestamp", std::chrono::system_clock::now().time_since_epoch().count()}
            };
            res.set_content(health_status.dump(), "application/json");
        });

        
        spdlog::trace("HTTP server endpoints registered");
        server_.listen_after_bind();
    }
};
}
