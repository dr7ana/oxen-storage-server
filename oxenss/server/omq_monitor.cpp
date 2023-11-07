#include "omq.h"
#include "utils.h"
#include "../common/namespace.h"
#include "../common/format.h"
#include "../rpc/client_rpc_endpoints.h"
#include "../utils/time.hpp"

#include <chrono>
#include <oxen/log.hpp>
#include <tuple>
#include <type_traits>
#include <utility>
#include <oxenc/bt_producer.h>
#include <oxenc/hex.h>
#include <sodium/crypto_sign.h>

namespace oxenss::server {

using namespace std::literals;

static auto logcat = log::Cat("monitor");

namespace {
    // Merges sorted vectors a and b together, returns the sorted, combined vector (but without any
    // duplicates).  We avoid reallocating the vectors when possible (i.e. if either is a subset of
    // the other).
    std::vector<namespace_id> merge_namespaces(
            std::vector<namespace_id> a, std::vector<namespace_id> b) {
        // If the first arg of b comes before a, then our only subset case can be b as a superset of
        // a, so swap arguments so that the subset case always involves `a` as the superset.
        if (!b.empty() && (a.empty() || b.front() < a.front()))
            a.swap(b);
        // Figure out if a is a superset of b (in which case we can just return a):
        auto ita = a.begin(), itb = b.begin();
        while (ita != a.end() && itb != b.end()) {
            if (*itb > *ita)
                ita++;  // We have an element only in a, which is fine, skip it
            else if (*itb == *ita) {
                ita++;  // The element is in both, which is fine.
                itb++;
            } else
                break;  // We found a b that isn't in a, so we don't have a subset.
        }
        if (itb == b.end())
            return a;  // We hit the end of b without any violations above, which means everything
                       // in b is already in a.

        // Otherwise we need to merge them into a new sorted container c:
        std::vector<namespace_id> c;
        ita = a.begin();
        itb = b.begin();
        while (ita != a.end() || itb != b.end()) {
            if (itb == b.end())
                c.push_back(*ita++);
            else if (ita == a.end())
                c.push_back(*itb++);
            else if (*ita < *itb)
                c.push_back(*ita++);
            else if (*ita == *itb) {
                c.push_back(*ita++);
                itb++;  // Value is in both vectors, but we only want it once
            } else
                c.push_back(*itb++);
        }
        return c;
    }

}  // namespace

void OMQ::update_monitors(
        std::vector<sub_info>& subs,
        std::optional<oxenmq::ConnectionID> omq,
        std::optional<std::shared_ptr<quic::Connection>> quic) {
    std::unique_lock lock{monitoring_mutex_};
    for (auto& [pubkey, pubkey_hex, namespaces, want_data] : subs) {
        bool found = false;
        for (auto [it, end] = monitoring_.equal_range(pubkey); it != end; ++it) {
            auto& mon_data = it->second;
            if ((omq and mon_data.push_conn == omq) || (quic and mon_data.quic == quic)) {
                mon_data.namespaces =
                        merge_namespaces(std::move(mon_data.namespaces), std::move(namespaces));
                log::debug(
                        logcat,
                        "monitor.messages sub renewed for {} monitoring namespace(s) {}",
                        pubkey_hex,
                        fmt::join(mon_data.namespaces, ", "));
                mon_data.reset_expiry();
                mon_data.want_data |= want_data;
                found = true;
                if (omq and not mon_data.push_conn)
                    mon_data.push_conn = omq;
                if (quic and not mon_data.quic)
                    mon_data.quic = quic;
                break;
            }
        }
        if (not found) {
            log::debug(
                    logcat,
                    "monitor.messages new subscription for {} monitoring namespace(s) {}",
                    pubkey_hex,
                    fmt::join(namespaces, ", "));
            monitoring_.emplace(
                    std::piecewise_construct,
                    std::forward_as_tuple(std::move(pubkey)),
                    std::forward_as_tuple(std::move(namespaces), want_data, omq, quic));
        }
    }
}

void OMQ::handle_monitor_messages(oxenmq::Message& message) {
    if (message.data.size() != 1 || message.data[0].size() < 2 ||
        !(message.data[0].front() == 'd' || message.data[0].front() == 'l') ||
        message.data[0].back() != 'e') {
        message.send_reply(oxenc::bt_serialize(oxenc::bt_dict{
                {"errcode", static_cast<int>(MonitorResponse::BAD_ARGS)},
                {"error",
                 "Invalid arguments: monitor.messages takes a single bencoded dict or list "
                 "parameter"}}));
        return;
    }
    const auto& m = message.data[0];

    std::string result;
    std::vector<sub_info> subs;
    try {
        if (m.front() == 'd') {
            oxenc::bt_dict_producer out;
            handle_monitor_message_single(oxenc::bt_dict_consumer{m}, out, subs);
            result = std::move(out).str();
        } else {
            oxenc::bt_list_producer out;
            oxenc::bt_list_consumer l{m};
            while (!l.is_finished())
                handle_monitor_message_single(l.consume_dict_consumer(), out.append_dict(), subs);
            result = std::move(out).str();
        }
    } catch (const std::exception& e) {
        message.send_reply(oxenc::bt_serialize(oxenc::bt_dict{
                {"errcode", static_cast<int>(MonitorResponse::BAD_ARGS)},
                {"error", "Invalid arguments: Failed to parse monitor.messages data value"}}));
        return;
    }

    if (not subs.empty())
        update_monitors(subs, message.conn);

    message.send_reply(result);
}

static void write_metadata(
        oxenc::bt_dict_producer& d, std::string_view pubkey, const message& msg) {
    d.append("@", pubkey);
    d.append("h", msg.hash);
    d.append("n", to_int(msg.msg_namespace));
    d.append("t", to_epoch_ms(msg.timestamp));
    d.append("z", to_epoch_ms(msg.expiry));
}

void OMQ::send_notifies(message msg) {
    auto pubkey = msg.pubkey.prefixed_raw();
    auto now = std::chrono::steady_clock::now();
    std::vector<oxenmq::ConnectionID> relay_to, relay_to_with_data;
    {
        std::shared_lock lock{monitoring_mutex_};
        for (auto [it, end] = monitoring_.equal_range(pubkey); it != end; ++it) {
            const auto& mon_data = it->second;
            if (mon_data.expiry >= now &&
                std::binary_search(
                        mon_data.namespaces.begin(), mon_data.namespaces.end(), msg.msg_namespace))
                (mon_data.want_data ? relay_to_with_data : relay_to).push_back(*mon_data.push_conn);
        }
    }

    if (relay_to.empty() && relay_to_with_data.empty())
        return;

    // We output a dict with keys (in order):
    // - @ pubkey
    // - h msg hash
    // - n msg namespace
    // - t msg timestamp
    // - z msg expiry
    // - ~ msg data (optional)
    constexpr size_t metadata_size = 2       // d...e
                                   + 3 + 36  // 1:@ and 33:[33-byte pubkey]
                                   + 3 + 46  // 1:h and 43:[43-byte base64 unpadded hash]
                                   + 3 + 8   // 1:n and i-32768e
                                   + 3 + 16  // 1:t and i1658784776010e plus a byte to grow
                                   + 3 + 16  // 1:z and i1658784776010e plus a byte to grow
                                   + 10;     // safety margin

    oxenc::bt_dict_producer d;
    d.reserve(
            relay_to_with_data.empty() ? metadata_size
                                       : metadata_size  // all the metadata above
                                                 + 3    // 1:~
                                                 + 8    // 76800: plus a couple bytes to grow
                                                 + msg.data.size());

    write_metadata(d, pubkey, msg);

    if (!relay_to.empty())
        for (const auto& conn : relay_to)
            omq_.send(conn, "notify.message", d.view());

    if (!relay_to_with_data.empty()) {
        d.append("~", msg.data);
        for (const auto& conn : relay_to_with_data)
            omq_.send(conn, "notify.message", d.view());
    }
}

}  // namespace oxenss::server
