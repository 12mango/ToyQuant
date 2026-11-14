#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "exchange/execution_report.h"
#include "exchange/matching_engine.h"
#include "exchange/order.h"
#include "lines/order_recorder.h"

// The two halves of the order leg's recording, and the pair is the whole reason it exists: a session that
// cannot be replayed cannot be checked, and every other claim in this repository is checked by re-running
// something (ADR 0005, decision 5).
//
// `RecordingEngine` is the writer. It is a decorator with the same shape as `PaperBroker` and `OrderGateway`:
// the pipeline, the strategy and the ledger do not know it is there, and with no prefix it is not constructed
// at all. What it adds to the venue below it is one file -- what went out and what came back, in the order it
// happened.
//
// `RecordedVenue` is the reader. It is an `IMatchingEngine` whose answers come from a file instead of from a
// venue: a submission is matched against the recording's own row and the reports recorded for it are emitted
// through the callback. It is what makes "the session's recording replays to the same [PNL] and [EXECUTION]"
// a comparison rather than a hope -- the second run's ledger is fed the first run's reports, field for field.
class RecordingEngine : public IMatchingEngine
{
   public:
    // The recording is `<prefix>_orders.csv`. A prefix that cannot be opened throws, because a run that
    // believed it was recording and was not is the failure this class exists to prevent.
    RecordingEngine(IMatchingEngine& venue, std::string prefix);

    void send_order(const exchange::Order& order) override;
    void cancel_order(uint64_t order_id) override;

    // Forwarded rather than swallowed: this decorator can sit around the in-process engine, which is a market
    // as well as a venue, so a replay that records its own fills still fills from the book. `LiveVenue` is the
    // one engine where these two are no-ops, and that is its own decision, not this class's.
    void process_bbo(const BboQuote& quote) override { venue_.process_bbo(quote); }
    void process_market_trade(const MarketTrade& trade) override { venue_.process_market_trade(trade); }

    void set_report_callback(ReportCallback cb) override;

    OrderRecorder& recorder() { return recorder_; }

   private:
    IMatchingEngine& venue_;
    OrderRecorder recorder_;
    ReportCallback callback_;
};

// Counters of a replayed session. `rows_left` is the one a check reads: a recording whose rows were not all
// replayed describes a session that ran longer than the run replaying it, and the two are then not the same
// session however equal the summaries look.
struct RecordedVenueCounters
{
    uint64_t submissions{0};
    uint64_t cancels{0};
    uint64_t reports{0};
    // Rows the run has not replayed yet: the unread ones plus the reports still held for the clock.
    uint64_t rows_left{0};
    // Held for the recorded clock: a venue answered at a time, and a replay that released every answer on
    // submission would act on a fill the live run had not heard of yet. This is the queue's current depth.
    uint64_t reports_pending{0};
    // Released by `flush` because the market data ended before the session did. Not an error -- a replay can be
    // shorter than the session -- but the number a reader needs to know the two were not the same length.
    uint64_t flushed_at_end{0};

    std::string to_log_string() const;
};

class RecordedVenue : public IMatchingEngine
{
   public:
    explicit RecordedVenue(std::string path);

    void send_order(const exchange::Order& order) override;
    void cancel_order(uint64_t order_id) override;

    // A recorded venue matches its own orders, exactly like a live one, so the two calls a simulation needs do
    // nothing here. Same smell as `LiveVenue`: the interface mixes "be a market" with "be a venue".
    void process_bbo(const BboQuote& quote) override { (void)quote; }
    void process_market_trade(const MarketTrade& trade) override { (void)trade; }

    void set_report_callback(ReportCallback cb) override { callback_ = std::move(cb); }

    const RecordedVenueCounters& counters() const { return counters_; }
    const std::string& path() const { return path_; }

    // The recorded clock. A report is released once the replay's own clock is past the timestamp it was recorded
    // with, so an answer arrives when the live session heard it rather than when the replay asked: without this a
    // replay that runs the book faster than the venue runs the strategy on fills it would not have had.
    void advance_to(uint64_t now_us);
    // Whatever is still held, released at the end of the run: the market data was shorter than the session. Counted
    // in `flushed_at_end`, because a replay shorter than its recording is a fact about the two inputs.
    void flush();

   private:
    // Queues the report rows recorded after `index`, which are the venue's answer to the call at that row, and
    // advances the cursor past them. They wait in `pending_` for the clock.
    void queue_reports_after(std::size_t index);
    void emit(const ExecutionReport& report);
    // Names the file and the line when a replay asks for something the recording does not contain. An error
    // rather than a count, because a recording and the run replaying it are supposed to be the same session.
    [[noreturn]] void mismatch(const OrderLogRow& row, const std::string& asked) const;


    std::string path_;
    std::vector<OrderLogRow> rows_;
    std::size_t cursor_{0};
    std::deque<std::pair<uint64_t, ExecutionReport>> pending_;
    RecordedVenueCounters counters_;
    ReportCallback callback_;
};
