#pragma once
#include <cstdint>
#include <string>
#include <string_view>

#include "lines/venue_order_translator.h"

// The order channel's session, without a socket: what to send when the connection comes up, what a frame means, what
// happens when it drops, and what the summary prints at the end. The transport is a `Sink` the caller supplies, so the
// whole state machine runs in one process under a test -- which is the claim the Python relay's stand-in venue makes
// today and a straight port of the socket code could not inherit.
//
// This is ADR 0006's third slice, state machine first: the login, the `orders` subscription, the ping cadence and the
// reconnect that logs in and subscribes again. Moving the TLS/WebSocket plumbing beside it is the step after, and the
// ADR says which is landed.
class OrderSession
{
   public:
    struct Credentials
    {
        std::string api_key;
        std::string secret;
        std::string passphrase;
    };

    // Where the session's output goes. Split in two because they are two different peers: the venue gets JSON frames
    // and the engine gets the adapter's lines, and a single method would let one be written where the other belongs.
    class Sink
    {
       public:
        virtual ~Sink() = default;
        virtual void to_venue(std::string_view frame) = 0;
        virtual void to_engine(std::string_view line) = 0;
    };

    struct Counters
    {
        std::uint64_t logins{0};
        std::uint64_t subscribes{0};
        std::uint64_t reconnects{0};
        std::uint64_t messages{0};
        // Entries the venue sent for an instrument this session did not trade. Counted rather than answered to the
        // engine: an account with two instruments must not have the second one's fills booked against the first.
        std::uint64_t other_instruments{0};
        std::uint64_t unreadable{0};

        std::string to_log_string() const;
    };

    OrderSession(std::string instrument, Credentials credentials, std::int64_t inst_code, OrderMapping mapping,
                 Sink& sink);

    // The connection is up. Emits the login frame for `now_seconds`, which is the venue's epoch seconds -- the login
    // signature is computed over exactly this string, so the caller decides the clock and the test can pin it.
    void on_connected(std::uint64_t now_seconds);

    // A frame from the venue: the login's answer, the subscription's, or an `orders` push.
    void on_frame(std::string_view frame);

    // A line from the engine, as `LiveVenue` wrote it to its transport.
    void on_engine_line(std::string_view line);

    // The same line as the venue's request without sending it, for the placement route that does not send a frame: the
    // session keeps one translator, so a REST body and a websocket frame cannot disagree about a size.
    OrderTranslator::Request request_of(std::string_view line) { return translator_.request_of(line); }

    // The connection went away. Counted, and the next `on_connected` logs in and subscribes again.
    void on_disconnected() { ++counters_.reconnects; }

    // The keepalive: true when a ping was due and has been sent.
    bool on_tick(std::uint64_t now_seconds);

    const Counters& counters() const { return counters_; }
    const OrderTranslator& translator() const { return translator_; }
    // The three credentials the private channel signs with. The placement route signs with the same three from the same
    // copy, which is why they are readable here rather than passed to two owners; nothing prints them.
    const Credentials& credentials() const { return credentials_; }
    // `logins=… subscribes=… reconnects=… messages=…`, the line the relay prints when the engine hangs up.
    std::string summary() const { return counters_.to_log_string(); }

   private:
    void subscribe();

    std::string instrument_;
    Credentials credentials_;
    OrderTranslator translator_;
    Sink& sink_;
    Counters counters_;
    std::uint64_t last_ping_seconds_{0};
};
