// The order leg's translation, ported from `tools/live/venue_relay.py` and pinned against the cases its `--self-test`
// states. Two things make this slice worth a test rather than a reading: the venue's sizes are decimal strings that a
// contract can fill in parts, and the preimage of every number is a grid the venue refuses rather than rounds.
//
// The expectations here are the relay's own, written as literals: a port that "improves" one of them has to change a
// line that says why it was what it was.

#include <cassert>
#include <string>
#include <vector>

#include "lines/venue_json.h"
#include "lines/venue_order_translator.h"

namespace
{
Json parse(std::string_view text)
{
    Json value;
    std::string error;
    JsonParser parser(text);
    const bool ok = parser.parse(value, error);
    assert(ok);
    return value;
}

std::vector<std::string> decode_text(OrderTranslator& translator, std::string_view text)
{
    return translator.decode(parse(text));
}
}  // namespace

int main()
{
    // The demo instrument's numbers, which are what the conversion is checked against.
    OrderTranslator translator("BTC-USDT-SWAP", 10459);

    // An order line becomes the venue's request: the engine's units in contracts, the price on the tick, and the two
    // names the venue needs -- its own instrument and its internal code for it.
    const std::string order = translator.encode("ORDER 7 BTC-PERPETUAL B 6421.5 100000 Gtc");
    assert(order == R"({"id":"7","op":"order","args":[{"instId":"BTC-USDT-SWAP","tdMode":"cross",)"
                    R"("side":"buy","ordType":"limit","px":"6421.5","sz":"10","clOrdId":"7","instIdCode":10459}]})");

    // Without the code the field is absent rather than zero: a venue that never heard of `instIdCode` must not be sent
    // a zero for it.
    OrderTranslator plain("BTC-PERPETUAL");
    assert(plain.encode("ORDER 7 BTC-PERPETUAL S 6421.5 100000 Gtc").find("instIdCode") == std::string::npos);

    // Add-liquidity-only reaches the venue as its own `ordType`, because a plain limit is what gets filled as a
    // taker when the book has moved through the price on the way. The word shares the slot a time in force occupies:
    // nothing is both post-only and an IOC, and the engine's line has one field for the kind of order.
    const std::string add_only = translator.encode("ORDER 12 BTC-PERPETUAL B 6421.5 100000 PostOnly");
    assert(add_only.find(R"("ordType":"post_only")") != std::string::npos);
    assert(add_only.find(R"("ordType":"limit")") == std::string::npos);
    // The other two kinds are words too: an IOC sent as a limit would rest at the venue, which is not what the flag
    // asks for. Both are OKX `ordType` values, and the engine's one slot is the only thing that changes.
    assert(translator.encode("ORDER 13 BTC-PERPETUAL B 6421.5 100000 Ioc").find(R"("ordType":"ioc")") !=
           std::string::npos);
    assert(translator.encode("ORDER 14 BTC-PERPETUAL B 6421.5 100000 Fok").find(R"("ordType":"fok")") !=
           std::string::npos);

    // A venue that cancels one of those has declined the fill, not ended a quote this client withdrew: the matcher
    // reports the same fact as a cancel whose reason is `post_only`, so one counter reads both modes.
    assert(decode_text(translator, R"({"arg":{"channel":"orders"},"data":[{"clOrdId":"12","state":"canceled",)"
                                 R"("accFillSz":"0"}]})") == std::vector<std::string>{"CANCELED 12 post_only"});


    assert(translator.encode("CANCEL 7") ==
           R"({"id":"7","op":"cancel-order","args":[{"instId":"BTC-USDT-SWAP","clOrdId":"7","instIdCode":10459}]})");

    // A close is a market order whose size rounds UP to the venue's grid and carries `reduceOnly`: a fractional
    // position (9,500 engine units = 0.95 of a contract) is closed by one contract, and the venue caps the order at
    // the position instead of flipping it. The request keeps the units so the acknowledgement can name the size the
    // engine asked to close rather than the venue's grid size.
    const auto close = translator.request_of("CLOSE 60 BTC-USDT-SWAP S 9500");
    assert(close.op == "order");
    assert(close.args_object ==
           R"({"instId":"BTC-USDT-SWAP","tdMode":"cross","side":"sell","ordType":"market","sz":"1","reduceOnly":true,"clOrdId":"60","instIdCode":10459})");
    assert(close.units == 9500);
    assert(close.reduce_only);
    assert(translator.contracts_to_close(9500) == 1);
    assert(translator.contracts_to_close(10000) == 1);
    assert(translator.contracts_to_close(10001) == 2);

    // A size below one contract is refused here with both numbers, not at the venue as a rejection: one contract is
    // 0.01 of the coin and the scale is a million units to the coin, so ten thousand units is one contract.
    try
    {
        translator.encode("ORDER 8 BTC-PERPETUAL B 6421.5 1 Gtc");
        assert(false && "a size below one contract was placed");
    }
    catch (const OrderError& error)
    {
        const std::string message = error.what();
        assert(message.find("1 engine units") != std::string::npos);
        assert(message.find("10000 units") != std::string::npos);
    }

    // Rounded down, never up: a size above what the strategy asked for is the direction that turns a demonstration
    // into a position.
    assert(translator.contracts_for(25000) == 2);
    assert(translator.contracts_for(10000) == 1);

    // A price one step off the venue's grid is refused rather than corrected, so the engine's double is rounded onto it
    // -- nearest step, which is the smallest change the venue can represent.
    assert(translator.price_on_tick("66664.54") == "66664.5");
    assert(translator.price_on_tick("6421.5") == "6421.5");

    // An acknowledgement says what is still working, in engine units, and is not a fill even when the venue's answer
    // already carries a cumulative count.
    const std::vector<std::string> ack = decode_text(
        translator, R"({"op":"order","id":"12","data":[{"clOrdId":"12","sCode":"0","sz":"10","accFillSz":"4"}]})");
    assert(ack.size() == 1 && ack[0] == "ACK 12 60000");

    // The `orders` channel repeats a cumulative count and the venue reports a contract's fill in parts, as a decimal.
    // One fill per increment, and a repeat is counted rather than booked twice.
    const auto push = [&translator](std::string_view acc, std::string_view fee = "")
    {
        std::string text = R"({"arg":{"channel":"orders"},"data":[{"clOrdId":"14","state":"partially_filled",)"
                           R"("fillPx":"66664.04","accFillSz":")";
        text += acc;
        text += "\"";
        if (!fee.empty())
        {
            text += R"(,"fillFee":")";
            text += fee;
            text += "\"";
        }
        text += "}]}";
        return decode_text(translator, text);
    };
    const std::vector<std::string> first = push("0.45");
    assert(first.size() == 1 && first[0] == "FILL 14 66664.04 4500");
    assert(push("0.45").empty());
    assert(translator.fills_deduplicated() == 1);
    const std::vector<std::string> second = push("0.69");
    assert(second.size() == 1 && second[0] == "FILL 14 66664.04 2400");

    // The venue's own fee rides on the fill, negated into the engine's sign; a rebate is the same field the other way.
    const std::vector<std::string> charged = push("0.9", "-0.14999409");
    assert(charged.size() == 1 && charged[0] == "FILL 14 66664.04 2100 0.14999409");
    OrderTranslator rebating("BTC-USDT-SWAP");
    assert(decode_text(rebating, R"({"arg":{"channel":"orders"},"data":[{"clOrdId":"15","state":"partially_filled",)"
                                 R"("fillPx":"100","accFillSz":"0.5","fillFee":"0.02"}]})") ==
           std::vector<std::string>{"FILL 15 100 5000 -0.02"});

    // The venue's own role rides on the fill when it reports one: `execType` is `M` for a fill that rested and `T`
    // for one that took. It is the sixth field, and a fill the venue reports no fee for states a zero to keep the
    // position, because the role is what picks the rate the account is charged.
    assert(decode_text(translator, R"({"arg":{"channel":"orders"},"data":[{"clOrdId":"21","state":"partially_filled",)"
                                 R"("fillPx":"100","accFillSz":"0.5","execType":"M"}]})") ==
           std::vector<std::string>{"FILL 21 100 5000 0 maker"});
    assert(decode_text(translator, R"({"arg":{"channel":"orders"},"data":[{"clOrdId":"22","state":"partially_filled",)"
                                 R"("fillPx":"100","accFillSz":"0.5","fillFee":"-0.01","execType":"T"}]})") ==
           std::vector<std::string>{"FILL 22 100 5000 0.01 taker"});
    // A push with no role keeps the shorter line, which is what the simulated venue sends and what every live number
    // before the role was carried was booked under.
    assert(decode_text(translator, R"({"arg":{"channel":"orders"},"data":[{"clOrdId":"23","state":"partially_filled",)"
                                 R"("fillPx":"100","accFillSz":"0.9"}]})") ==
           std::vector<std::string>{"FILL 23 100 9000"});

    // A counter that goes backwards is not a duplicate to ignore: it would book a position nothing explains.
    try
    {
        push("0.4");
        assert(false && "a fill count that went backwards was accepted");
    }
    catch (const OrderError& error)
    {
        assert(std::string(error.what()).find("went backwards") != std::string::npos);
    }

    // A refusal at the top level reaches the engine as a REJECT rather than as silence, and one with no id cannot be
    // attributed to an order, so it is refused as unreadable.
    assert(decode_text(translator, R"({"id":"7","op":"order","code":"50014","msg":"bad","data":[]})") ==
           std::vector<std::string>{"REJECT 7 venue"});
    try
    {
        decode_text(translator, R"({"id":"","op":"order","code":"50014","data":[]})");
        assert(false && "a refusal with no id was answered to nobody");
    }
    catch (const OrderError&)
    {
    }

    // A message this channel does not know is an error rather than an empty answer.
    try
    {
        decode_text(translator, R"({"arg":{"channel":"positions"},"data":[{"clOrdId":"1"}]})");
        assert(false && "an unknown channel was accepted");
    }
    catch (const OrderError&)
    {
    }

    return 0;
}
