#pragma once







#include <winsock2.h>
#include <ws2tcpip.h>
#include <spdlog/spdlog.h>
#include <string>
#include <atomic>
#include <cstring>
#include <stdexcept>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace socks5_tunnel {



extern "C" {
    extern volatile int      g_socks5_active;          
    extern sockaddr_in       g_socks5_relay_addr;      
    extern SOCKET            g_socks5_control_socket;  
}


struct TcpConn {
    SOCKET s = INVALID_SOCKET;
    sockaddr_in peer_addr{};   // the proxy's address, used for the 0.0.0.0 relay fallback
    explicit TcpConn(const std::string& host, uint16_t port) {
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        char ps[8]; snprintf(ps, sizeof(ps), "%u", (unsigned)port);
        if (getaddrinfo(host.c_str(), ps, &hints, &res) != 0 || !res)
            throw std::runtime_error("SOCKS5: DNS failed for " + host);
        s = ::socket(res->ai_family, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) { freeaddrinfo(res); throw std::runtime_error("SOCKS5: socket()"); }
        DWORD tv = 10000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);

        // Bound the TCP connect. SO_RCVTIMEO/SO_SNDTIMEO do NOT apply to connect(), so an
        // unreachable proxy previously blocked this thread for the OS default (~21 s on
        // Windows). This runs from the ENet Connection event, i.e. on the relay thread,
        // so an unbounded stall here starves both ENet hosts and looks like a random
        // disconnect. Use a non-blocking connect + select with a hard deadline.
        u_long nonblocking = 1;
        ioctlsocket(s, FIONBIO, &nonblocking);

        const int rc = ::connect(s, res->ai_addr, (int)res->ai_addrlen);
        const int lastError = WSAGetLastError();
        freeaddrinfo(res);

        bool connected = (rc == 0);
        if (!connected && lastError == WSAEWOULDBLOCK) {
            fd_set writeSet;
            FD_ZERO(&writeSet);
            FD_SET(s, &writeSet);

            timeval timeout{};
            timeout.tv_sec  = 10;
            timeout.tv_usec = 0;

            const int sel = ::select(0, nullptr, &writeSet, nullptr, &timeout);
            if (sel > 0) {
                int soError = 0;
                int len = sizeof(soError);
                if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&soError, &len) == 0 && soError == 0) {
                    connected = true;
                }
            }
        }

        if (!connected) {
            closesocket(s); s = INVALID_SOCKET;
            throw std::runtime_error("SOCKS5: TCP connect failed or timed out");
        }

        nonblocking = 0;
        ioctlsocket(s, FIONBIO, &nonblocking);

        // Remember the proxy address we are actually connected to; the UDP ASSOCIATE
        // reply may legitimately tell us to relay through this same address.
        int peerLen = sizeof(peer_addr);
        if (getpeername(s, (struct sockaddr*)&peer_addr, &peerLen) != 0) {
            memset(&peer_addr, 0, sizeof(peer_addr));
        }
    }
    ~TcpConn() { if (s != INVALID_SOCKET) closesocket(s); }
    void tx(const uint8_t* b, int n) {
        for (int sent = 0; sent < n;) {
            int r = ::send(s, (const char*)b + sent, n - sent, 0);
            if (r <= 0) throw std::runtime_error("SOCKS5: send failed");
            sent += r;
        }
    }
    void rx(uint8_t* b, int n) {
        for (int got = 0; got < n;) {
            int r = ::recv(s, (char*)b + got, n - got, 0);
            if (r <= 0) throw std::runtime_error("SOCKS5: recv failed");
            got += r;
        }
    }
};


inline bool is_active() { return g_socks5_active != 0; }

inline void disconnect() {
    g_socks5_active = 0;
    if (g_socks5_control_socket != INVALID_SOCKET) {
        closesocket(g_socks5_control_socket);
        g_socks5_control_socket = INVALID_SOCKET;
    }
    memset(&g_socks5_relay_addr, 0, sizeof(g_socks5_relay_addr));
}



inline void connect(const std::string& host, uint16_t port,
                    const std::string& user = "", const std::string& pass = "")
{
    disconnect(); 

    TcpConn tc(host, port);
    bool use_auth = !user.empty() && !pass.empty();

    
    {
        std::vector<uint8_t> hello = { 0x05 };
        if (use_auth) { hello.push_back(0x02); hello.push_back(0x00); hello.push_back(0x02); }
        else          { hello.push_back(0x01); hello.push_back(0x00); }
        tc.tx(hello.data(), (int)hello.size());
        uint8_t r[2]; tc.rx(r, 2);
        if (r[0] != 0x05) throw std::runtime_error("SOCKS5: bad version");
        if (r[1] == 0xFF) throw std::runtime_error("SOCKS5: no acceptable auth method");
    }

    
    if (use_auth) {
        std::vector<uint8_t> a;
        a.push_back(0x01);
        a.push_back((uint8_t)user.size());
        for (char c : user)  a.push_back((uint8_t)c);
        a.push_back((uint8_t)pass.size());
        for (char c : pass)  a.push_back((uint8_t)c);
        tc.tx(a.data(), (int)a.size());
        uint8_t r[2]; tc.rx(r, 2);
        if (r[1] != 0x00) throw std::runtime_error("SOCKS5: authentication failed");
    }

    
    sockaddr_in relay{};
    {
        uint8_t req[10] = { 0x05, 0x03, 0x00, 0x01,
                            0x00,0x00,0x00,0x00, 0x00,0x00 };
        tc.tx(req, 10);
        uint8_t r[4]; tc.rx(r, 4);
        if (r[0] != 0x05) throw std::runtime_error("SOCKS5: bad version in reply");
        if (r[1] != 0x00) throw std::runtime_error(
            std::string("SOCKS5: UDP ASSOCIATE error code=") + std::to_string(r[1]));
        if (r[3] != 0x01) throw std::runtime_error("SOCKS5: only IPv4 relay supported");
        uint8_t tail[6]; tc.rx(tail, 6);
        relay.sin_family = AF_INET;
        memcpy(&relay.sin_addr, tail,     4);
        memcpy(&relay.sin_port, tail + 4, 2); 
    }

    // Fallback the SOCKS5 spec allows and many servers rely on: a reply address of
    // 0.0.0.0:0 means "the relay is at the same address you reached over TCP". Without
    // this, every wrapped datagram is sent to 0.0.0.0 and dropped, so the upstream ENet
    // handshake can never complete -- the tunnel reports success and then nothing works.
    {
        const bool addr_unspecified = (relay.sin_addr.s_addr == 0);
        const bool port_unspecified = (relay.sin_port == 0);

        if (addr_unspecified || port_unspecified) {
            char reply_ip[INET_ADDRSTRLEN] = { 0 };
            inet_ntop(AF_INET, &relay.sin_addr, reply_ip, sizeof(reply_ip));

            if (addr_unspecified) {
                relay.sin_addr = tc.peer_addr.sin_addr;
            }
            if (port_unspecified) {
                relay.sin_port = tc.peer_addr.sin_port;
            }

            char fixed_ip[INET_ADDRSTRLEN] = { 0 };
            inet_ntop(AF_INET, &relay.sin_addr, fixed_ip, sizeof(fixed_ip));
            spdlog::warn("[SOCKS5] relay reply was {}:0 -- many proxies mean \"same address as "
                         "the control connection\"; substituting {}:{}",
                         reply_ip, fixed_ip, ntohs(relay.sin_port));
        }

        char relay_ip[INET_ADDRSTRLEN] = { 0 };
        inet_ntop(AF_INET, &relay.sin_addr, relay_ip, sizeof(relay_ip));
        spdlog::info("[SOCKS5] UDP relay address = {}:{}", relay_ip, ntohs(relay.sin_port));
    }

    
    g_socks5_relay_addr    = relay;
    g_socks5_control_socket = tc.s;
    tc.s = INVALID_SOCKET;  
    g_socks5_active = 1;
}

} 
