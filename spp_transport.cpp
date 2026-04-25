#include "spp_transport.h"
#include "utils.h"

#include <cstring>
#include <cerrno>
#include <stdexcept>
#include <algorithm>
#include <chrono>

#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>
#include <bluetooth/rfcomm.h>

// ------------------------------------------------------------
// Helpers
// ------------------------------------------------------------
static bool is_mac_addr(const std::string &s)
{
    if (s.size() != 17) return false;
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2) { if (s[i] != ':') return false; }
        else { if (!isxdigit((unsigned char)s[i])) return false; }
    }
    return true;
}

static bool str_to_addr(const std::string &s, bdaddr_t &out)
{
    return str2ba(s.c_str(), &out) == 0;
}

// ------------------------------------------------------------
// SppTransport
// ------------------------------------------------------------
SppTransport::SppTransport()  = default;
SppTransport::~SppTransport() { stop_notify(); disconnect(); }

// Query RFCOMM channel via SDP
int SppTransport::find_channel(const std::string &mac)
{
    bdaddr_t target, any = {{0,0,0,0,0,0}};
    if (!str_to_addr(mac, target)) return 1;

    sdp_session_t *session = sdp_connect(&any, &target, SDP_RETRY_IF_BUSY);
    if (!session) {
        LOG_VERBOSE("SPP: SDP unavailable, using default channel 1");
        return 1;
    }

    uuid_t spp_uuid;
    sdp_uuid16_create(&spp_uuid, SERIAL_PORT_SVCLASS_ID);
    sdp_list_t *search = sdp_list_append(nullptr, &spp_uuid);
    uint32_t range = 0x0000ffff;
    sdp_list_t *attrid = sdp_list_append(nullptr, &range);

    sdp_list_t *rsp = nullptr;
    int channel = 1;

    if (sdp_service_search_attr_req(session, search,
                                    SDP_ATTR_REQ_RANGE, attrid, &rsp) == 0) {
        for (sdp_list_t *r = rsp; r; r = r->next) {
            sdp_record_t *rec = (sdp_record_t *)r->data;
            sdp_list_t *protos = nullptr;
            if (sdp_get_access_protos(rec, &protos) == 0) {
                int ch = sdp_get_proto_port(protos, RFCOMM_UUID);
                if (ch > 0) channel = ch;
                sdp_list_free(protos, nullptr);
            }
            sdp_record_free(rec);
        }
    }

    sdp_list_free(rsp, nullptr);
    sdp_list_free(search, nullptr);
    sdp_list_free(attrid, nullptr);
    sdp_close(session);
    LOG_VERBOSE("SPP: SDP channel=%d", channel);
    return channel;
}

// ------------------------------------------------------------
// connect_by_addr - raw RFCOMM socket
// ------------------------------------------------------------
bool SppTransport::connect_by_addr(const std::string &mac_addr, int channel)
{
    if (!is_mac_addr(mac_addr)) {
        LOG_ERROR("SPP: '%s' is not a MAC address", mac_addr.c_str());
        return false;
    }

    sock_fd_ = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
    if (sock_fd_ < 0) {
        LOG_ERROR("SPP: Cannot create RFCOMM socket: %s", strerror(errno));
        return false;
    }

    struct sockaddr_rc addr = {};
    addr.rc_family  = AF_BLUETOOTH;
    addr.rc_channel = (uint8_t)channel;
    if (!str_to_addr(mac_addr, addr.rc_bdaddr)) {
        ::close(sock_fd_); sock_fd_ = -1;
        return false;
    }

    LOG_INFO("SPP: Connecting to %s ch%d...", mac_addr.c_str(), channel);
    if (::connect(sock_fd_, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int err = errno;
        LOG_ERROR("SPP: RFCOMM connect failed: %s (errno=%d)", strerror(err), err);
        if (err == ECONNREFUSED || err == EACCES || err == EPERM) {
            LOG_ERROR("SPP: Pair the device first:");
            LOG_ERROR("       bluetoothctl pair %s", mac_addr.c_str());
            LOG_ERROR("       bluetoothctl trust %s", mac_addr.c_str());
        } else if (err == ETIMEDOUT) {
            LOG_ERROR("SPP: Timed out - is the printer on and in range?");
        }
        ::close(sock_fd_); sock_fd_ = -1;
        return false;
    }

    // Set socket non-blocking for the reader thread so it can be interrupted
    int flags = fcntl(sock_fd_, F_GETFL, 0);
    fcntl(sock_fd_, F_SETFL, flags | O_NONBLOCK);

    address_ = mac_addr;
    LOG_INFO("SPP: Connected via RFCOMM ch%d.", channel);
    return true;
}

// ------------------------------------------------------------
// scan_and_connect (MAC only - no name scan)
// ------------------------------------------------------------
bool SppTransport::scan_and_connect(const std::string &name_or_addr, int /*timeout*/)
{
    if (!is_mac_addr(name_or_addr)) return false;
    int ch = find_channel(name_or_addr);
    return connect_by_addr(name_or_addr, ch);
}

// ------------------------------------------------------------
// write
// ------------------------------------------------------------
void SppTransport::write(const Bytes &data)
{
    if (sock_fd_ < 0) throw std::runtime_error("SPP: Not connected");
    LOG_VERBOSE("SPP Write [%zu bytes]", data.size());

    // Socket is non-blocking; use blocking write via temporary flag toggle
    int flags = fcntl(sock_fd_, F_GETFL, 0);
    fcntl(sock_fd_, F_SETFL, flags & ~O_NONBLOCK);

    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::write(sock_fd_, data.data() + sent, data.size() - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            fcntl(sock_fd_, F_SETFL, flags);
            throw std::runtime_error(std::string("SPP write: ") + strerror(errno));
        }
        sent += (size_t)n;
    }

    fcntl(sock_fd_, F_SETFL, flags);
}

void SppTransport::write_chunked(const Bytes &data, size_t chunk_size, unsigned delay_ms)
{
    for (size_t off = 0; off < data.size(); off += chunk_size) {
        size_t end = std::min(off + chunk_size, data.size());
        Bytes chunk(data.begin() + off, data.begin() + end);
        write(chunk);
        if (delay_ms > 0 && end < data.size())
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
}

// ------------------------------------------------------------
// start_notify / stop_notify
// Non-blocking read with poll() so stop_notify() can interrupt.
// ------------------------------------------------------------
void SppTransport::start_notify(NotifyCallback cb)
{
    if (sock_fd_ < 0) return;
    notify_cb_      = cb;
    notify_running_ = true;
    notify_thread_  = std::thread([this]() {
        uint8_t buf[512];
        while (notify_running_) {
            // Poll with 50ms timeout so we can check notify_running_
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(sock_fd_, &rfds);
            struct timeval tv = {0, 50000};  // 50ms
            int sel = select(sock_fd_ + 1, &rfds, nullptr, nullptr, &tv);
            if (sel < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (sel == 0) continue;  // timeout, loop to check notify_running_

            ssize_t n = ::read(sock_fd_, buf, sizeof(buf));
            if (n > 0 && notify_cb_) {
                Bytes data(buf, buf + n);
                LOG_VERBOSE("SPP recv [%zd bytes]: %s", n, hex_dump(data).c_str());
                notify_cb_(data);
            } else if (n == 0) {
                LOG_VERBOSE("SPP: connection closed by device");
                break;
            } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
                LOG_VERBOSE("SPP read: %s", strerror(errno));
                break;
            }
        }
    });
}

void SppTransport::stop_notify()
{
    notify_running_ = false;
    // Thread uses select() with 50ms timeout - it will exit naturally
    if (notify_thread_.joinable())
        notify_thread_.join();
}

// ------------------------------------------------------------
// is_connected / disconnect
// ------------------------------------------------------------
bool SppTransport::is_connected() const { return sock_fd_ >= 0; }

void SppTransport::disconnect()
{
    if (sock_fd_ >= 0) {
        ::close(sock_fd_);
        sock_fd_ = -1;
    }
    LOG_INFO("SPP: Disconnected.");
}
