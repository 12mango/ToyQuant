#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "exchange/execution_report.h"
#include "exchange/order.h"

/*
-------------------------------------------
 OrderRecorder
-------------------------------------------

Writes the order leg of a session -- what went out and what came back -- to `<prefix>_orders.csv`, so a
live session can be replayed and compared instead of believed. This is step three of ADR 0005 and the
second recorder in the tree: `MarketEventRecorder` writes market data in the shapes the replay already
reads, and this writes the venue's side in a shape of its own, because an execution report has no column
in the replay formats and inventing one there would put a venue's field into a format a second reader
has to keep in step with.

    seq,kind,order_id,symbol,side,price,qty,ts,exec_type,cancel_reason,reject_reason,liquidity_role,fee,owner,tif

`kind` is `order` (a submission), `cancel` or `report`. A submission records what the strategy asked for
and a report records what the venue answered, field for field, which is what makes the reader's claim
possible: `RecordedVenue` replays this file as an engine and has to produce the same reports rather than
similar ones. Only a report fills the report columns -- `exec_type`, the two reasons, `liquidity_role`,
`fee` -- so a submission leaves them empty and a cancel carries an id and zeros: a reader cannot mistake a
default for something the venue said.

What the file carries as time is the session's own clock -- the event a submission was decided at and the
timestamp a report arrived with -- because that is what a replay has to agree with. A wall-clock time
would make two runs of the same recording differ.

A value the format cannot carry is refused rather than written approximately: a symbol or an owner holding
the comma the file splits on, a price that does not round-trip through the shortest decimal, or a report
whose type is not one of the six. The reader refuses the same things from the other side, naming the file
and the line, because a recording with a hole in it replays to a session that never happened.
*/

// One line per row, so `wc -l` and this line have to agree: submissions + cancels + reports rows after the
// header, and nothing else in the file.
struct OrderLogSummary
{
    uint64_t submissions{0};
    uint64_t cancels{0};
    uint64_t reports{0};

    std::string to_log_string() const;
};

class OrderRecorder
{
   public:
    // The recording is `<prefix>_orders.csv`. An empty prefix is an error rather than a file called
    // `_orders.csv`, and the directory is created if it is missing, for the same reason the market
    // recorder creates its own: a recording is written next to what it describes.
    explicit OrderRecorder(std::string prefix);

    // The three things a venue leg contains. Each is one row, and the order they are called in is the
    // order the file keeps, so a replay reads the session the way it happened.
    void record_submission(const exchange::Order& order);
    void record_cancel(uint64_t order_id);
    void record_report(const ExecutionReport& report);

    void flush();

    const OrderLogSummary& summary() const { return summary_; }
    const std::string& path() const { return path_; }

   private:
    void write_row(std::string row);
    void require_writable(std::string_view value, const char* what) const;

    std::string prefix_;
    std::string path_;
    std::ofstream out_;
    uint64_t sequence_{0};
    OrderLogSummary summary_;
};

enum class OrderLogKind : uint8_t
{
    Submission = 0,
    Cancel,
    Report
};

// One row of a recording, read back. `line` is the line it came from, so a disagreement between a
// replay and its recording can name where in the file the two parted company.
struct OrderLogRow
{
    OrderLogKind kind{OrderLogKind::Report};
    uint64_t line{0};
    uint64_t order_id{0};
    std::string symbol;
    exchange::Side side{exchange::Side::Buy};
    double price{0.0};
    uint64_t quantity{0};
    uint64_t ts{0};
    ExecType exec_type{ExecType::Resting};
    CancelReason cancel_reason{CancelReason::None};
    RejectReason reject_reason{RejectReason::None};
    LiquidityRole liquidity_role{LiquidityRole::Unknown};
    double fee{0.0};
    std::string owner;
    TimeInForce time_in_force{TimeInForce::Gtc};
};

// Reads a recording. Throws std::runtime_error naming the file and the line for a row that cannot be
// read -- a missing column, a kind that is not one of the three, a sequence number out of order, a token
// that is not in its table -- rather than skipping it.
std::vector<OrderLogRow> read_order_log(const std::string& path);
