#include "Core/Transport/MdnsAdvertiser.h"
#include "Core/Common/Logging.h"
#include <chrono>
#include <algorithm>
#include <string_view>

namespace {

using CarPlayStation::Core::Transport::MdnsPacketBuilder;

// Decode a DNS name (with RFC 1035 compression) starting at offset `start` in `buf`.
// On success, sets `outName` (dotted, e.g. "foo._carplay-ctrl._tcp.local.") and
// `outEnd` (offset just past the name, i.e. where the next field begins) and returns true.
// Handles compression pointers (0xC0xx) and guards against pointer loops / out-of-bounds.
bool DecodeDnsName(const uint8_t* buf, size_t len, size_t start, std::string& outName, size_t& outEnd) {
    std::string name;
    size_t cursor = start;
    size_t endPos = 0; // set to the resume position (after the 2-byte pointer) when we jump
    int labels = 0;

    while (cursor < len) {
        uint8_t first = buf[cursor];
        if (first == 0) {
            cursor++;
            break;
        }
        if (first & 0xC0) {
            // Compression pointer
            if (cursor + 2 > len) return false;
            uint16_t pointer = (static_cast<uint16_t>(first & 0x3F) << 8) | buf[cursor + 1];
            if (endPos == 0) {
                endPos = cursor + 2; // resume here after the pointer in the original stream
            }
            cursor = pointer;
            if (cursor >= len) return false;
            if (++labels > 64) return false; // guard against pointer loops
            continue;
        }
        // Regular label
        size_t labelLen = first;
        if (cursor + 1 + labelLen > len) return false;
        name.append(reinterpret_cast<const char*>(buf + cursor + 1), labelLen);
        name.push_back('.');
        cursor += 1 + labelLen;
        if (++labels > 64) return false;
    }

    outName = name;
    outEnd = (endPos != 0) ? endPos : cursor;
    return true;
}

// What we care about in an inbound mDNS datagram.
struct MdnsInspection {
    uint16_t port = 0;             // _carplay-ctrl SRV port (0 when absent)
    std::string ctrlInstance;      // _carplay-ctrl instance from a PTR answer
    bool airplayQuery = false;     // somebody is looking for our AirPlay service
    std::string summary;           // decoded names, for logging
};

// Inspect an inbound mDNS/DNS datagram.
//
// The old parser only recognised an SRV record whose own name contained
// "_carplay-ctrl", so a perfectly correct DNS-SD *browse* answer (a PTR record
// pointing at the instance name) was thrown away and the port stayed 0. This
// walks the whole packet and reports both halves of the lookup, plus whether the
// datagram is a query for our _airplay._tcp service. It also builds a decoded
// summary so the receive path is observable in the log.
MdnsInspection InspectMdnsPacket(const uint8_t* buf, size_t len) {
    MdnsInspection out;
    if (len < 12) return out;

    // 12-byte header: ID(2) FLAGS(2) QDCOUNT(2) ANCOUNT(2) NSCOUNT(2) ARCOUNT(2)
    const bool isResponse = (buf[2] & 0x80) != 0;
    uint16_t qdCount = (static_cast<uint16_t>(buf[4]) << 8) | buf[5];
    uint16_t anCount = (static_cast<uint16_t>(buf[6]) << 8) | buf[7];
    uint16_t nsCount = (static_cast<uint16_t>(buf[8]) << 8) | buf[9];
    uint16_t arCount = (static_cast<uint16_t>(buf[10]) << 8) | buf[11];

    auto typeName = [](uint16_t t) -> const char* {
        switch (t) {
            case MdnsPacketBuilder::kDnsTypeA: return "A";
            case MdnsPacketBuilder::kDnsTypePtr: return "PTR";
            case MdnsPacketBuilder::kDnsTypeTxt: return "TXT";
            case MdnsPacketBuilder::kDnsTypeAaaa: return "AAAA";
            case MdnsPacketBuilder::kDnsTypeSrv: return "SRV";
            default: return "OTHER";
        }
    };

    std::string& summary = out.summary;
    summary = isResponse ? "response" : "query";
    summary += " qd=" + std::to_string(qdCount) + " an=" + std::to_string(anCount)
            + " ns=" + std::to_string(nsCount) + " ar=" + std::to_string(arCount);

    size_t cursor = 12;

    // QDCOUNT questions: NAME + QTYPE(2) + QCLASS(2)
    for (uint16_t i = 0; i < qdCount; ++i) {
        std::string qname;
        size_t qend = 0;
        if (!DecodeDnsName(buf, len, cursor, qname, qend)) return out;
        cursor = qend;
        if (cursor + 4 > len) return out;
        uint16_t qtype = (static_cast<uint16_t>(buf[cursor]) << 8) | buf[cursor + 1];
        cursor += 4;

        summary += " | q:" + qname + "/" + typeName(qtype);

        // The iPhone browses _airplay._tcp (or the service enumeration / any
        // record) before it will talk to us; answer all of those forms.
        if (qname.find("_airplay._tcp") != std::string::npos ||
            qname == "local." || qname == "." ||
            qname.find("_services._dns-sd") != std::string::npos) {
            out.airplayQuery = true;
        }
    }

    // ANSWER/AUTHORITY/ADDITIONAL records: NAME + TYPE + CLASS + TTL + RDLENGTH + RDATA
    const uint16_t rrCount = static_cast<uint16_t>(anCount + nsCount + arCount);
    for (uint16_t i = 0; i < rrCount; ++i) {
        std::string rname;
        size_t rend = 0;
        if (!DecodeDnsName(buf, len, cursor, rname, rend)) return out;
        cursor = rend;
        if (cursor + 10 > len) return out;
        uint16_t type = (static_cast<uint16_t>(buf[cursor]) << 8) | buf[cursor + 1];
        uint16_t rdLength = (static_cast<uint16_t>(buf[cursor + 8]) << 8) | buf[cursor + 9];
        cursor += 10;
        if (cursor + rdLength > len) return out;

        if (type == MdnsPacketBuilder::kDnsTypeSrv && rdLength >= 6) {
            // SRV RDATA: PRIORITY(2) WEIGHT(2) PORT(2) TARGET(name)
            uint16_t port = (static_cast<uint16_t>(buf[cursor + 4]) << 8) | buf[cursor + 5];
            summary += " | srv:" + rname + ":" + std::to_string(port);
            if (rname.find("_carplay-ctrl") != std::string::npos && port > 0) {
                out.port = port;
                if (out.ctrlInstance.empty()) out.ctrlInstance = rname;
            }
        } else if (type == MdnsPacketBuilder::kDnsTypePtr && rdLength >= 2) {
            // PTR RDATA is a single (possibly compressed) target name.
            std::string target;
            size_t tend = 0;
            if (DecodeDnsName(buf, len, cursor, target, tend)) {
                summary += " | ptr:" + rname + "->" + target;
                if (rname.find("_carplay-ctrl") != std::string::npos && !target.empty()) {
                    if (out.ctrlInstance.empty()) out.ctrlInstance = target;
                }
            }
        } else {
            summary += " | rr:" + rname + "/" + typeName(type);
        }

        cursor += rdLength;
    }

    return out;
}

} // namespace

namespace CarPlayStation::Core::Transport {

MdnsAdvertiser::MdnsAdvertiser() = default;

MdnsAdvertiser::~MdnsAdvertiser() {
    Stop();
}

Common::Status MdnsAdvertiser::Start(const MdnsServiceConfig& config) {
    // If already running, refresh the stored config (e.g. to apply the real
    // device identity) and re-announce so peers re-discover with the new
    // identity. Do not tear down the socket / worker thread.
    if (m_running.load()) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_config = config;
        }
        (void)BroadcastAnnouncement();
        return Common::Status::Ok();
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_config = config;
    }

    if (m_simulatedMode.load()) {
        m_running = true;
        (void)BroadcastAnnouncement();
        m_workerThread = std::thread(&MdnsAdvertiser::BroadcastWorkerLoop, this);
        return Common::Status::Ok();
    }

#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return Common::Status::Error(Common::StatusCode::Unavailable, "WSAStartup failed");
    }

    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == INVALID_SOCKET) {
        int err = WSAGetLastError();
        return Common::Status::Error(Common::StatusCode::Unavailable, "Failed to create UDP socket: code " + std::to_string(err));
    }

    BOOL reuse = TRUE;
    setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    BOOL bcast = TRUE;
    setsockopt(m_socket, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&bcast), sizeof(bcast));

    // Bind to 0.0.0.0:5353 so all outgoing mDNS packets have source port 5353 per RFC 6762
    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(5353);
    bindAddr.sin_addr.s_addr = INADDR_ANY;
    if (bind(m_socket, reinterpret_cast<const sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        CPS_LOG_WARN("MdnsAdvertiser", "bind() to 0.0.0.0:5353 failed (code ", std::to_string(err), "), trying interface IP ", config.ipv4Address);
        if (!config.ipv4Address.empty() && config.ipv4Address != "0.0.0.0") {
            inet_pton(AF_INET, config.ipv4Address.c_str(), &bindAddr.sin_addr);
            if (bind(m_socket, reinterpret_cast<const sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCKET_ERROR) {
                int err2 = WSAGetLastError();
                CPS_LOG_ERROR("MdnsAdvertiser", "bind() to ", config.ipv4Address, ":5353 failed (code ", std::to_string(err2), ")");
            } else {
                CPS_LOG_INFO("MdnsAdvertiser", "bind() OK on ", config.ipv4Address, ":5353");
            }
        }
    } else {
        CPS_LOG_INFO("MdnsAdvertiser", "bind() OK on 0.0.0.0:5353");
    }

    // Join multicast group 224.0.0.251
    ip_mreq mreq{};
    inet_pton(AF_INET, "224.0.0.251", &mreq.imr_multiaddr);
    if (!config.ipv4Address.empty() && config.ipv4Address != "0.0.0.0") {
        inet_pton(AF_INET, config.ipv4Address.c_str(), &mreq.imr_interface);
    } else {
        mreq.imr_interface.s_addr = INADDR_ANY;
    }
    int mjoin = setsockopt(m_socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&mreq), sizeof(mreq));
    if (mjoin == SOCKET_ERROR) {
        CPS_LOG_WARN("MdnsAdvertiser", "IP_ADD_MEMBERSHIP failed (code ", std::to_string(WSAGetLastError()), ")");
    } else {
        CPS_LOG_INFO("MdnsAdvertiser", "Joined mDNS multicast group 224.0.0.251 on ", config.ipv4Address);
    }

    // Multicast TTL = 255 per RFC 6762
    unsigned char ttl = 255;
    setsockopt(m_socket, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl));

    m_running = true;
    (void)BroadcastAnnouncement();
    m_workerThread = std::thread(&MdnsAdvertiser::BroadcastWorkerLoop, this);

    return Common::Status::Ok();
#else
    return Common::Status::Error(Common::StatusCode::NotImplemented, "mDNS native socket only supported on Windows");
#endif
}

void MdnsAdvertiser::Stop() {
    m_running = false;

#ifdef _WIN32
    if (m_socket != INVALID_SOCKET) {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }
#endif

    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

bool MdnsAdvertiser::IsRunning() const noexcept {
    return m_running.load();
}

MdnsServiceConfig MdnsAdvertiser::GetConfig() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_config;
}

Common::Status MdnsAdvertiser::BroadcastAnnouncement() {
    MdnsServiceConfig cfg;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cfg = m_config;
    }

    auto packet = MdnsPacketBuilder::BuildAirPlayAnnouncement(cfg);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastPacket = packet;
        m_broadcastCount++;
    }

    if (m_simulatedMode.load()) {
        return Common::Status::Ok();
    }

    return SendRawMulticast(Common::ByteSpan(packet.data(), packet.size()));
}

Common::Status MdnsAdvertiser::AnnounceTo(const std::string& targetIp) {
    if (m_simulatedMode.load()) {
        return BroadcastAnnouncement();
    }
#ifdef _WIN32
    if (!targetIp.empty() && m_socket != INVALID_SOCKET) {
        MdnsServiceConfig cfg;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            cfg = m_config;
        }
        auto packet = MdnsPacketBuilder::BuildAirPlayAnnouncement(cfg);
        sockaddr_in targetAddr{};
        targetAddr.sin_family = AF_INET;
        targetAddr.sin_port = htons(5353);
        if (inet_pton(AF_INET, targetIp.c_str(), &targetAddr.sin_addr) == 1) {
            int n1 = sendto(m_socket, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
                            reinterpret_cast<const sockaddr*>(&targetAddr), sizeof(targetAddr));

            // Also query targeted _carplay-ctrl._tcp.local.
            auto query = MdnsPacketBuilder::BuildCarPlayControlQuery();
            int n2 = sendto(m_socket, reinterpret_cast<const char*>(query.data()), static_cast<int>(query.size()), 0,
                            reinterpret_cast<const sockaddr*>(&targetAddr), sizeof(targetAddr));
            CPS_LOG_INFO("MdnsAdvertiser", "AnnounceTo: sent AirPlay announcement (", std::to_string(n1),
                         " bytes) and _carplay-ctrl query (", std::to_string(n2), " bytes) to ", targetIp, ":5353");
        }
    }
#endif
    return BroadcastAnnouncement();
}

Common::Status MdnsAdvertiser::SendRawMulticast(Common::ByteSpan packet) {
#ifdef _WIN32
    if (m_socket == INVALID_SOCKET) {
        return Common::Status::Error(Common::StatusCode::InvalidState, "UDP socket not open");
    }

    sockaddr_in mcastAddr{};
    mcastAddr.sin_family = AF_INET;
    mcastAddr.sin_port = htons(5353);
    inet_pton(AF_INET, "224.0.0.251", &mcastAddr.sin_addr);

    // Target the Hotspot / local interface (e.g. 192.168.137.1)
    if (!m_config.ipv4Address.empty() && m_config.ipv4Address != "0.0.0.0") {
        in_addr ifAddr{};
        if (inet_pton(AF_INET, m_config.ipv4Address.c_str(), &ifAddr) == 1) {
            setsockopt(m_socket, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&ifAddr), sizeof(ifAddr));
        }
    }

    int sent = sendto(m_socket, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
                      reinterpret_cast<const sockaddr*>(&mcastAddr), sizeof(mcastAddr));
    if (sent == SOCKET_ERROR) {
        int err = WSAGetLastError();
        return Common::Status::Error(Common::StatusCode::IoError, "Multicast sendto failed: code " + std::to_string(err));
    }

    // Subnet broadcast to 192.168.137.255:5353
    if (!m_config.ipv4Address.empty() && m_config.ipv4Address != "0.0.0.0") {
        std::string bcastIp = m_config.ipv4Address;
        auto lastDot = bcastIp.rfind('.');
        if (lastDot != std::string::npos) {
            bcastIp = bcastIp.substr(0, lastDot) + ".255";
            sockaddr_in bcastAddr{};
            bcastAddr.sin_family = AF_INET;
            bcastAddr.sin_port = htons(5353);
            if (inet_pton(AF_INET, bcastIp.c_str(), &bcastAddr.sin_addr) == 1) {
                sendto(m_socket, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
                       reinterpret_cast<const sockaddr*>(&bcastAddr), sizeof(bcastAddr));
            }
        }
    }
    return Common::Status::Ok();
#else
    (void)packet;
    return Common::Status::Error(Common::StatusCode::NotImplemented, "SendRawMulticast not implemented on non-Windows");
#endif
}

void MdnsAdvertiser::ClearDialedTargets() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_dialedTargets.clear();
    m_ctrlInstance.clear();
}

void MdnsAdvertiser::SendCtrlLookup(const std::string& instanceName) {
#ifdef _WIN32
    if (m_socket == INVALID_SOCKET) return;
    auto query = MdnsPacketBuilder::BuildCarPlayControlLookup(instanceName);
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(5353);
    inet_pton(AF_INET, "224.0.0.251", &dst.sin_addr);
    int sent = sendto(m_socket, reinterpret_cast<const char*>(query.data()),
                      static_cast<int>(query.size()), 0,
                      reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
    if (sent == SOCKET_ERROR) {
        CPS_LOG_WARN("MdnsAdvertiser", "SRV/A query for ", instanceName,
                     " failed (WSA error ", std::to_string(WSAGetLastError()), ")");
    } else {
        CPS_LOG_INFO("MdnsAdvertiser", "Sent SRV/A query for _carplay-ctrl instance ", instanceName);
    }
#else
    (void)instanceName;
#endif
}

void MdnsAdvertiser::DialCarPlayControl(const std::string& host, uint16_t port) {
#ifdef _WIN32
    if (host.empty() || port == 0) return;

    std::string key = host + ":" + std::to_string(port);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (std::find(m_dialedTargets.begin(), m_dialedTargets.end(), key) != m_dialedTargets.end()) {
            return;
        }
        m_dialedTargets.push_back(key);
    }

    std::string devId;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        devId = m_config.deviceId;
    }
    // The AirPlay-Receiver-Device-ID header must be a DECIMAL u64, not a hex
    // string. The reference (carplayd rx_connect.rs) converts the MAC via
    // mac_to_dec(): fold each octet as (v << 8) | octet.
    devId.erase(std::remove(devId.begin(), devId.end(), ':'), devId.end());
    unsigned long long devDec = 0;
    for (size_t i = 0; i < devId.size(); i += 2) {
        std::string oct = devId.substr(i, 2);
        unsigned long long v = 0;
        for (char c : oct) {
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned long long>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned long long>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned long long>(c - 'A' + 10);
        }
        devDec = (devDec << 8) | v;
    }
    std::string devDecStr = std::to_string(devDec);

    std::thread([host, port, devDecStr]() {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return;

        DWORD timeout = 3000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
            closesocket(s);
            return;
        }

        if (connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0) {
            std::string req = "GET /ctrl-int/1/connect HTTP/1.1\r\n"
                              "Host: " + host + ":" + std::to_string(port) + "\r\n"
                              "User-Agent: AirPlay/320.17\r\n"
                              "AirPlay-Receiver-Device-ID: " + devDecStr + "\r\n"
                              "Connection: keep-alive\r\n\r\n";

            send(s, req.data(), static_cast<int>(req.size()), 0);
            CPS_LOG_INFO("MdnsAdvertiser", "Sent /ctrl-int/1/connect to iPhone at ", host, ":", std::to_string(port), " (devId=", devDecStr, ")");

            char buf[256];
            int n = recv(s, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                CPS_LOG_INFO("MdnsAdvertiser", "iPhone /ctrl-int/1/connect response: ", std::string(buf, (std::min)(n, 64)));
            }
        } else {
            CPS_LOG_WARN("MdnsAdvertiser", "Failed to connect to iPhone control port ", host, ":", std::to_string(port));
        }
        closesocket(s);
    }).detach();
#else
    (void)host; (void)port;
#endif
}

void MdnsAdvertiser::BroadcastWorkerLoop() {
    while (m_running.load()) {
        (void)BroadcastAnnouncement();

#ifdef _WIN32
        if (m_socket != INVALID_SOCKET) {
            // Multicast query for _carplay-ctrl._tcp.local.
            auto query = MdnsPacketBuilder::BuildCarPlayControlQuery();
            SendRawMulticast(Common::ByteSpan(query.data(), query.size()));

            // Listen for responses using select() with 1s timeout
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(m_socket, &readfds);

            timeval tv{};
            tv.tv_sec = 1;
            tv.tv_usec = 0;

            int res = select(0, &readfds, nullptr, nullptr, &tv);
            if (res > 0 && FD_ISSET(m_socket, &readfds)) {
                // Drain all packets available in this window (the targeted query from
                // AnnounceTo may have produced a response on the same socket).
                for (int drain = 0; drain < 8; ++drain) {
                    sockaddr_in fromAddr{};
                    int fromLen = sizeof(fromAddr);
                    uint8_t rxBuf[2048];
                    int bytesRead = recvfrom(m_socket, reinterpret_cast<char*>(rxBuf), sizeof(rxBuf), 0,
                                             reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);
                    if (bytesRead <= 12) {
                        break;
                    }
                    char ipStr[INET_ADDRSTRLEN] = {0};
                    inet_ntop(AF_INET, &fromAddr.sin_addr, ipStr, sizeof(ipStr));

                    // Everything we receive is logged: without this there is no way
                    // to tell "the iPhone sends no mDNS" from "our socket never gets it".
                    MdnsInspection rx = InspectMdnsPacket(rxBuf, static_cast<size_t>(bytesRead));
                    CPS_LOG_INFO("MdnsAdvertiser", "mDNS inbound ", bytesRead, " bytes from ", ipStr, ":",
                                 std::to_string(ntohs(fromAddr.sin_port)), " [", rx.summary, "]");

                    // A browse answer gives us the instance name but not the port;
                    // chase it with an SRV/A query (RFC 6767 two-step resolution).
                    if (rx.port == 0 && !rx.ctrlInstance.empty()) {
                        std::string known;
                        {
                            std::lock_guard<std::mutex> lock(m_mutex);
                            known = m_ctrlInstance;
                        }
                        if (known != rx.ctrlInstance) {
                            {
                                std::lock_guard<std::mutex> lock(m_mutex);
                                m_ctrlInstance = rx.ctrlInstance;
                            }
                            SendCtrlLookup(rx.ctrlInstance);
                        }
                    }

                    if (rx.port > 0) {
                        bool already_dialed = false;
                        {
                            std::lock_guard<std::mutex> lock(m_mutex);
                            std::string key = std::string(ipStr) + ":" + std::to_string(rx.port);
                            already_dialed = (std::find(m_dialedTargets.begin(), m_dialedTargets.end(), key) != m_dialedTargets.end());
                        }
                        if (!already_dialed) {
                            CPS_LOG_INFO("MdnsAdvertiser", "Discovered iPhone _carplay-ctrl service at ", ipStr, ":", std::to_string(rx.port));
                            DialCarPlayControl(ipStr, rx.port);
                        }
                    }

                    // Respond to AirPlay service queries from the iPhone.
                    // The iPhone queries _airplay._tcp.local. to discover our
                    // RTSP endpoint (SRV -> port 5000). Without a response the
                    // iPhone never learns where to connect for video.
                    if (rx.airplayQuery) {
                        CPS_LOG_INFO("MdnsAdvertiser", "Received AirPlay service query from ", ipStr, " — responding with SRV/TXT/A records");
                        MdnsServiceConfig cfg;
                        {
                            std::lock_guard<std::mutex> lock(m_mutex);
                            cfg = m_config;
                        }
                        auto response = MdnsPacketBuilder::BuildAirPlayAnnouncement(cfg);
                        // RFC 6762 §6.2: mDNS responses MUST be sent to the
                        // multicast group 224.0.0.251:5353, NOT unicast to the
                        // querier. The iPhone's mDNSResponder only listens on the
                        // multicast group, so a unicast reply is never seen and
                        // the iPhone keeps re-querying forever.
                        sockaddr_in respAddr{};
                        respAddr.sin_family = AF_INET;
                        respAddr.sin_port = htons(5353);
                        inet_pton(AF_INET, "224.0.0.251", &respAddr.sin_addr);
                        int sent = sendto(m_socket, reinterpret_cast<const char*>(response.data()),
                                          static_cast<int>(response.size()), 0,
                                          reinterpret_cast<const sockaddr*>(&respAddr), sizeof(respAddr));
                        if (sent == SOCKET_ERROR) {
                            CPS_LOG_WARN("MdnsAdvertiser", "Failed to send AirPlay mDNS response to multicast (WSA error ", std::to_string(WSAGetLastError()), ")");
                        }
                    }
                }
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
#endif
    }
}

void MdnsAdvertiser::EnableSimulatedMode(bool enable) {
    m_simulatedMode = enable;
}

bool MdnsAdvertiser::IsSimulatedMode() const noexcept {
    return m_simulatedMode.load();
}

size_t MdnsAdvertiser::GetBroadcastCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_broadcastCount;
}

std::optional<Common::ByteBuffer> MdnsAdvertiser::GetLastBroadcastPacket() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_lastPacket.empty()) {
        return std::nullopt;
    }
    return m_lastPacket;
}

} // namespace CarPlayStation::Core::Transport
