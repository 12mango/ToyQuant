// The order channel's session, driven in one process: the login it emits is checked against the signature a stand-in
// venue computes from the same secret, the subscription follows the venue's answer rather than a timer, a drop forces a
// new login and a new subscription, and an account's channel is filtered to the instrument this session trades.
//
// The stand-in is the assertion: the session builds the login from `venue_auth`, and the check recomputes it from the
// secret the venue holds. A test that compared the session with itself would agree with any wrong preimage.

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include "lines/venue_auth.h"
#include "lines/venue_json.h"
#include "lines/venue_order_session.h"

namespace
{
class RecordingSink : public OrderSession::Sink
{
   public:
    void to_venue(std::string_view frame) override { venue.emplace_back(frame); }
    void to_engine(std::string_view line) override { engine.emplace_back(line); }

    std::vector<std::string> venue;
    std::vector<std::string> engine;
};

Json parse(std::string_view text)
{
    Json value;
    std::string error;
    JsonParser parser(text);
    const bool ok = parser.parse(value, error);
    assert(ok);
    return value;
}

std::string field(const Json& object, std::string_view key)
{
    const Json* found = object.find(key);
    return found == nullptr ? std::string() : found->as_text();
}

// What a stand-in venue does with the login it received: the timestamp and the secret are its own, so the signature is
// recomputed here rather than read back from the frame.
bool login_is_signed(const Json& login, std::string_view secret)
{
    const Json* args = login.find("args");
    if (args == nullptr || args->items.empty()) return false;
    const Json& entry = args->items.front();
    const std::string timestamp = field(entry, "timestamp");
    const std::string sign = field(entry, "sign");
    return !timestamp.empty() && sign == venue_auth::login_signature(timestamp, secret);
}
}  // namespace

int main()
{
    const std::string secret = "toyquant-relay-self-test-secret";
    RecordingSink sink;
    OrderSession session("BTC-USDT-SWAP", OrderSession::Credentials{.api_key = "key-1",
                                                                    .secret = secret,
                                                                    .passphrase = "pass-1"},
                         10459, OrderMapping{}, sink);

    // The connection comes up on the venue's clock, which is the string the signature is over.
    session.on_connected(1794528000);
    assert(sink.venue.size() == 1);
    const Json login = parse(sink.venue[0]);
    assert(field(login, "op") == "login");
    assert(login_is_signed(login, secret));
    // And on this timestamp the signature is the relay's pinned vector, which is the same value its Python self-test
    // checks: two implementations agreeing on one preimage is the only thing that makes a login a login.
    assert(field(login.find("args")->items.front(), "sign") == "WIUsY7IKdr05lNBq2VgPKZu5ME3ZgT1i6w4ixCiJ9W8=");
    assert(session.counters().logins == 1);

    // The subscription goes out when the venue answers the login, not before: a subscribe on an unauthenticated socket
    // is refused, which is why the state machine waits for the event rather than a timer.
    session.on_frame(R"({"event":"login","code":"0"})");
    assert(sink.venue.size() == 2);
    assert(sink.venue[1] == R"({"op":"subscribe","args":[{"channel":"orders","instType":"ANY"}]})");
    assert(session.counters().subscribes == 1);

    // An order line from the engine becomes the venue's request on the same frame the translator produces, so the
    // session adds no second translation.
    session.on_engine_line("ORDER 7 BTC-PERPETUAL B 6421.5 100000 Gtc");
    assert(sink.venue.size() == 3);
    assert(sink.venue[2] == R"({"id":"7","op":"order","args":[{"instId":"BTC-USDT-SWAP","tdMode":"cross",)"
                          R"("side":"buy","ordType":"limit","px":"6421.5","sz":"10","clOrdId":"7","instIdCode":10459}]})");

    // An account's `orders` channel carries every instrument it trades. The session answers the engine for its own and
    // counts the rest, because a fill booked against the wrong symbol is a position nobody can explain.
    session.on_frame(R"({"arg":{"channel":"orders"},"data":[)"
                     R"({"clOrdId":"14","instId":"BTC-USDT-SWAP","state":"partially_filled",)"
                     R"("fillPx":"66664.04","accFillSz":"0.45","fillFee":"-0.14999409"},)"
                     R"({"clOrdId":"15","instId":"ETH-USDT-SWAP","state":"filled","fillPx":"3000","accFillSz":"1"}]})");
    assert(sink.engine.size() == 1);
    assert(sink.engine[0] == "FILL 14 66664.04 4500 0.14999409");
    assert(session.counters().other_instruments == 1);

    // A drop is a new session at the venue: the next connection logs in again and subscribes again, and the counters
    // are what say the reconnect path ran rather than the first connection lasting.
    session.on_disconnected();
    assert(session.counters().reconnects == 1);
    session.on_connected(1794528100);
    assert(sink.venue.size() == 4);
    const Json second_login = parse(sink.venue[3]);
    assert(login_is_signed(second_login, secret));
    assert(field(second_login.find("args")->items.front(), "timestamp") == "1794528100");
    assert(field(second_login.find("args")->items.front(), "sign") !=
           "WIUsY7IKdr05lNBq2VgPKZu5ME3ZgT1i6w4ixCiJ9W8=");
    session.on_frame(R"({"event":"login","code":"0"})");
    assert(sink.venue.size() == 5);
    assert(sink.venue[4] == R"({"op":"subscribe","args":[{"channel":"orders","instType":"ANY"}]})");
    assert((session.counters().logins == 2 && session.counters().subscribes == 2));

    // The keepalive is a cadence, not a frame per event: nothing at nineteen seconds, a ping at twenty.
    assert(!session.on_tick(1794528119));
    assert(session.on_tick(1794528120));
    assert(sink.venue.back() == R"({"op":"ping"})");
    assert(!session.on_tick(1794528120));

    // A message the session cannot read is counted rather than answered, and the summary names every counter the relay
    // prints when the engine hangs up.
    session.on_frame("{not json");
    assert(session.counters().unreadable == 1);
    const std::string summary = session.summary();
    assert(summary.find("logins=2") != std::string::npos);
    assert(summary.find("subscribes=2") != std::string::npos);
    assert(summary.find("reconnects=1") != std::string::npos);
    assert(summary.find("other_instruments=1") != std::string::npos);
    assert(summary.find("unreadable=1") != std::string::npos);

    return 0;
}
