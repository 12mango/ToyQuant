// Step three of ADR 0005: the order leg written down and read back. The check is the recording's contract --
// what the venue said, written to a file and replayed as an engine, is the same report sequence field for
// field -- plus the refusals that make a recording a record rather than a summary: a field the format cannot
// carry is refused when it is written, and a row that cannot be read is an error naming the file and the line
// rather than a gap nobody notices.

#include <cassert>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "exchange/matching_engine.h"
#include "lines/order_recorder.h"
#include "lines/recorded_venue.h"
#include "support/test_support.h"

namespace
{
// A venue whose answers are scripted, so the recorder is exercised on a session holding every kind of report
// -- a resting acknowledgement, a fill with a rebate, a rejection and a cancel -- rather than on whatever a
// book happened to fill in a window.
class ScriptedVenue : public IMatchingEngine
{
   public:
    void send_order(const exchange::Order& order) override
    {
        if (order.id == 4)
        {
            ExecutionReport rejected;
            rejected.order_id = order.id;
            rejected.symbol = order.symbol;
            rejected.side = order.side;
            rejected.exec_type = ExecType::Rejected;
            rejected.reject_reason = RejectReason::Venue;
            rejected.ts = order.ts;
            emit(rejected);
            return;
        }
        ExecutionReport resting;
        resting.order_id = order.id;
        resting.symbol = order.symbol;
        resting.side = order.side;
        resting.price = order.price;
        resting.quantity = order.qty;
        resting.ts = order.ts;
        resting.exec_type = ExecType::Resting;
        emit(resting);

        ExecutionReport trade;
        trade.order_id = order.id;
        trade.symbol = order.symbol;
        trade.side = order.side;
        trade.price = order.price;
        trade.quantity = order.qty / 2;
        trade.ts = order.ts + 5;
        trade.exec_type = ExecType::Trade;
        trade.liquidity_role = LiquidityRole::Maker;
        // Negative on purpose: an equities rebate is a fee the run receives, and a recording that lost the sign
        // would turn every rebate into a charge when it was replayed.
        trade.fee = -0.04;
        emit(trade);
    }

    void cancel_order(uint64_t order_id) override
    {
        ExecutionReport cancelled;
        cancelled.order_id = order_id;
        cancelled.exec_type = ExecType::Cancelled;
        cancelled.cancel_reason = CancelReason::User;
        // After the fill above and before the next order, so the scripted session's clock moves forward the way a
        // real one does; the replay releases its queue as a prefix, in file order.
        cancelled.ts = 110;
        emit(cancelled);
    }

    void process_bbo(const BboQuote& quote) override { (void)quote; }
    void process_market_trade(const MarketTrade& trade) override { (void)trade; }

    void set_report_callback(ReportCallback cb) override { callback_ = std::move(cb); }

   private:
    void emit(const ExecutionReport& report)
    {
        if (callback_) callback_(report);
    }

    ReportCallback callback_;
};

exchange::Order make_order(uint64_t id, exchange::Side side, double price, uint64_t quantity, uint64_t ts,
                           std::string symbol = "BTC-PERPETUAL")
{
    exchange::Order order;
    order.id = id;
    order.symbol = std::move(symbol);
    order.side = side;
    order.type = exchange::OrderType::Limit;
    order.price = price;
    order.qty = quantity;
    order.remaining = quantity;
    order.ts = ts;
    order.time_in_force = TimeInForce::Gtc;
    return order;
}

// The same four calls the recording was made over, so a replay and its recording are asked the same questions.
void drive_session(IMatchingEngine& venue)
{
    venue.send_order(make_order(7, exchange::Side::Buy, 6421.5, 10, 100));
    venue.cancel_order(7);
    venue.send_order(make_order(4, exchange::Side::Sell, 6420.0, 5, 200));
    venue.send_order(make_order(9, exchange::Side::Sell, 6419.5, 3, 250));
}

// Every field a report carries, because a replay that reproduced the type and the id but lost the fee would
// pass a weaker comparison and then fail the ledger it was standing in for.
bool same_report(const ExecutionReport& left, const ExecutionReport& right)
{
    return left.order_id == right.order_id && left.symbol == right.symbol && left.side == right.side &&
           left.exec_type == right.exec_type && left.price == right.price &&
           left.quantity == right.quantity && left.ts == right.ts && left.owner == right.owner &&
           left.liquidity_role == right.liquidity_role && left.fee == right.fee &&
           left.reject_reason == right.reject_reason && left.cancel_reason == right.cancel_reason;
}

std::vector<std::string> lines_of(const std::string& path)
{
    std::ifstream input(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) lines.push_back(line);
    return lines;
}

// True when `message` names the file and the line, which is the form every refusal in this pair takes.
bool names(const std::string& message, const std::string& path, std::size_t line)
{
    return message.find(path) != std::string::npos &&
           message.find(path + ":" + std::to_string(line) + ":") != std::string::npos;
}
}  // namespace

int main()
{
    test_support::TempDir root("order_recorder_test");
    const std::string prefix = (root / "session").string();
    const std::string log_path = prefix + "_orders.csv";

    // The recording: three submissions, one cancel, and the six reports the scripted venue answered with.
    std::vector<ExecutionReport> recorded;
    {
        ScriptedVenue venue;
        RecordingEngine engine(venue, prefix);
        engine.set_report_callback([&recorded](const ExecutionReport& report) { recorded.push_back(report); });
        drive_session(engine);
        engine.recorder().flush();
        assert(engine.recorder().summary().submissions == 3);
        assert(engine.recorder().summary().cancels == 1);
        assert(engine.recorder().summary().reports == 6);
        assert(engine.recorder().path() == log_path);
    }
    assert(recorded.size() == 6);

    // The file says what the summary says: a header, then one row per call and one per report, and nothing else.
    const std::vector<std::string> written = lines_of(log_path);
    assert(written.size() == 11);
    assert(written[0] ==
           "seq,kind,order_id,symbol,side,price,qty,ts,exec_type,cancel_reason,reject_reason,liquidity_role,"
           "fee,owner,tif");
    // Pinned rather than described: a column that moved would still parse, and these two lines are what say it
    // did not move.
    assert(written[1] == "0,order,7,BTC-PERPETUAL,B,6421.5,10,100,,,,,,,gtc");
    assert(written[2] == "1,report,7,BTC-PERPETUAL,B,6421.5,10,100,resting,none,none,unknown,0,,");
    assert(written[3] == "2,report,7,BTC-PERPETUAL,B,6421.5,5,105,trade,none,none,maker,-0.04,,");
    assert(written[4] == "3,cancel,7,,,0,0,0,,,,,0,,");

    // The replay: the same four calls against the file instead of the venue, and the reports have to be the same
    // sequence field for field -- but on the recorded clock. Every answer is held until the run's own clock is past
    // the timestamp it was recorded with, which is the amendment ADR 0005 records: a pull replay would hand the
    // strategy a fill the live session had not heard yet.
    {
        RecordedVenue replay(log_path);
        std::vector<ExecutionReport> replayed;
        replay.set_report_callback([&replayed](const ExecutionReport& report) { replayed.push_back(report); });
        drive_session(replay);
        // All six are queued and none is due: the recording's first answer is at ts=100 and this run has not declared
        // a clock yet.
        assert(replayed.empty());
        assert(replay.counters().reports == 0);
        assert(replay.counters().reports_pending == 6);

        replay.advance_to(99);
        assert(replayed.empty());
        // The clock passes the acknowledgement's own time; the session booked it during that event, so the first
        // decision that saw it was the next one's.
        replay.advance_to(100);
        assert(replayed.empty());
        replay.advance_to(101);
        assert(replayed.size() == 1 && replayed.front().exec_type == ExecType::Resting);
        replay.advance_to(105);
        assert(replayed.size() == 1);
        replay.advance_to(106);  // its fill, recorded at 105
        assert(replayed.size() == 2 && replayed.back().exec_type == ExecType::Trade);
        // The run ended before the recording did: the rest is released at the end and counted, rather than lost.
        assert(replay.counters().flushed_at_end == 0);
        replay.flush();
        assert(replayed.size() == recorded.size());
        for (std::size_t index = 0; index < replayed.size(); ++index)
            assert(same_report(replayed[index], recorded[index]));
        assert(replay.counters().submissions == 3);
        assert(replay.counters().cancels == 1);
        assert(replay.counters().reports == 6);
        assert(replay.counters().flushed_at_end == 4);
        assert(replay.counters().rows_left == 0);
    }

    // A run whose clock reaches every recorded answer needs no flush, and its counters say so.
    {
        RecordedVenue replay(log_path);
        std::vector<ExecutionReport> replayed;
        replay.set_report_callback([&replayed](const ExecutionReport& report) { replayed.push_back(report); });
        drive_session(replay);
        replay.advance_to(1000);
        assert(replayed.size() == 6);
        assert(replay.counters().flushed_at_end == 0);
        assert(replay.counters().rows_left == 0);
    }

    // A report's timestamp is the order's or the trade's own, not the moment the session booked it, so a recording's
    // report times are not monotone: two live orders interleave. The queue is released as a prefix, in file order, so
    // the sequence the session booked is the sequence a replay books -- a report whose own time has passed does not
    // jump ahead of the row before it.
    {
        const std::string interleaved = (root / "interleaved").string() + "_orders.csv";
        {
            std::ofstream out(interleaved);
            out << "seq,kind,order_id,symbol,side,price,qty,ts,exec_type,cancel_reason,reject_reason,"
                   "liquidity_role,fee,owner,tif\n";
            out << "0,order,7,BTC-PERPETUAL,B,6421.5,10,100,,,,,,,gtc\n";
            out << "1,report,7,BTC-PERPETUAL,B,6421.5,10,200,resting,none,none,unknown,0,,\n";
            out << "2,report,7,BTC-PERPETUAL,B,6421.5,5,150,trade,none,none,maker,0,,\n";
        }
        RecordedVenue replay(interleaved);
        std::vector<ExecutionReport> replayed;
        replay.set_report_callback([&replayed](const ExecutionReport& report) { replayed.push_back(report); });
        replay.send_order(make_order(7, exchange::Side::Buy, 6421.5, 10, 100));
        // The clock reaches the second report's time but not the first's, and the prefix holds: nothing is delivered
        // out of order.
        replay.advance_to(150);
        assert(replayed.empty());
        replay.advance_to(201);
        assert(replayed.size() == 2);
        assert(replayed[0].exec_type == ExecType::Resting && replayed[1].exec_type == ExecType::Trade);
    }

    // A replay that stops short of the recording's end is visible rather than silently fine: this is the count
    // that tells a reader the two sessions are not the same one.
    {
        RecordedVenue partial(log_path);
        partial.send_order(make_order(7, exchange::Side::Buy, 6421.5, 10, 100));
        // Two rows are held for the clock and seven more are unread, so nine of the file's ten have not reached this
        // run: that is the count which says the recording and the replay are not the same session.
        assert(partial.counters().rows_left == 9);
        assert(partial.counters().reports_pending == 2);
    }

    // A field the format cannot carry is refused when it is written, not escaped or truncated.
    {
        const std::string refused_prefix = (root / "refused").string();
        ScriptedVenue venue;
        RecordingEngine engine(venue, refused_prefix);
        bool refused = false;
        try
        {
            engine.send_order(make_order(1, exchange::Side::Buy, 10.0, 1, 1, "BTC,PERP"));
        }
        catch (const std::invalid_argument& error)
        {
            refused = std::string(error.what()).find("symbol") != std::string::npos;
        }
        assert(refused);
    }

    // A row that cannot be read is an error naming the file and the line, never a skipped row. The kind here is
    // one this protocol does not have, which is what a venue's new field would look like to an old reader.
    {
        const std::string broken = (root / "broken").string() + "_orders.csv";
        {
            std::ofstream out(broken);
            out << "seq,kind,order_id,symbol,side,price,qty,ts,exec_type,cancel_reason,reject_reason,"
                   "liquidity_role,fee,owner,tif\n"
                << "0,fill,7,BTC-PERPETUAL,B,6421.5,10,100,,,,,0,,\n";
        }
        bool refused = false;
        try
        {
            read_order_log(broken);
        }
        catch (const std::runtime_error& error)
        {
            refused = names(error.what(), broken, 2);
        }
        assert(refused);
    }

    // A replay asked for something the recording does not hold names where the two parted company, rather than
    // answering from a different order's reports.
    {
        RecordedVenue replay(log_path);
        bool refused = false;
        try
        {
            replay.send_order(make_order(7, exchange::Side::Buy, 6000.0, 10, 100));
        }
        catch (const std::runtime_error& error)
        {
            refused = names(error.what(), log_path, 2);
        }
        assert(refused);
    }

    // A recording that is not there is an error naming the file, not a venue that answered nothing.
    {
        const std::string missing = (root / "missing").string() + "_orders.csv";
        bool refused = false;
        try
        {
            RecordedVenue replay(missing);
            (void)replay;
        }
        catch (const std::runtime_error& error)
        {
            refused = std::string(error.what()).find(missing) != std::string::npos;
        }
        assert(refused);
    }

    return 0;
}
