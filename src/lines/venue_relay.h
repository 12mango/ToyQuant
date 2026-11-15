#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// The C++ twin of `tools/live/venue_relay.py`: a client to a venue's public stream, a server to the engine,
// and a direct recorder, all over one decoder. It is a line component rather than core because it knows
// a venue's message shapes, which the engine deliberately does not.
//
// TLS is a compile-time option. With OpenSSL's headers present CMake defines TOYQUANT_HAVE_OPENSSL and
// a `wss://` venue is spoken directly; without them the relay speaks plaintext `ws://`, which is the
// local relay's own role, and the Python relay is kept as the TLS terminator.
struct RelayConfig
{
    std::string venue{"wss://ws.okx.com:8443/ws/v5/public"};
    std::string instrument{"BTC-USDT-SWAP"};
    std::string books_channel{"books"};
    std::size_t levels{25};
    double contract_size{0.01};
    uint64_t quantity_scale{1000000};
    // Where to serve the engine; empty disables serving.
    std::string listen{"127.0.0.1:8765"};
    // The order leg: serve the engine's order channel here and dial the venue's private channel. Serving it returns when
    // the engine hangs up, which is the shape the Python relay has, so a session that needs both legs runs two of these.
    std::string order_listen;
    std::string order_venue{"wss://wspap.okx.com:443/ws/v5/private?brokerId=9999"};
    // The placement surface, when it is REST rather than the socket's trade operation: this venue answers `50026` to the
    // websocket op and places over REST, so the two are not a choice of taste. Empty places over the socket.
    std::string order_rest;
    // The venue's own name for the contract, sent only when it is known (-1 leaves it out).
    int64_t order_inst_code{-1};
    // The two venue grids an order has to land on; the market side reads the venue's prices as they arrive.
    double tick_size{0.1};
    int64_t lot_size{1};
    // The venue's raw messages, appended, for a later --replay.
    std::string capture;
    // A prefix: the decoded rows are written to <prefix>_trades.csv and <prefix>_depth.csv.
    std::string record;
    // Serve this capture instead of connecting; no network.
    std::string replay;
    // Wall-clock seconds; 0 runs until stopped.
    double max_seconds{0.0};
    // Send a ping every N seconds even while data flows; 0 keeps the idle-only keepalive. The socket
    // polls every 200 ms, so the cadence is decided from the clock rather than from the timeout.
    double ping_seconds{0.0};
    // Close and reopen the venue connection every N seconds, for the reconnect test; 0 never drops.
    double drop_after_seconds{0.0};
};

// 0 on success. Throws std::runtime_error for a configuration or transport failure the caller should
// report, which `relay_main.cpp` turns into "Error: ..." and a non-zero exit, like the rest of the tools.
int run_relay(const RelayConfig& config);

// Decodes the message shapes a live capture showed and checks the rows and the gap count.
int relay_self_test();
