#include "lines/recorded_venue.h"

#include <stdexcept>
#include <string>
#include <utility>

namespace
{
const char* side_text(exchange::Side side)
{
    return side == exchange::Side::Buy ? "B" : "S";
}
}  // namespace

std::string RecordedVenueCounters::to_log_string() const
{
    return "submissions=" + std::to_string(submissions) + " cancels=" + std::to_string(cancels) +
           " reports=" + std::to_string(reports) + " rows_left=" + std::to_string(rows_left) +
           " pending=" + std::to_string(reports_pending) + " flushed=" + std::to_string(flushed_at_end);
}

RecordingEngine::RecordingEngine(IMatchingEngine& venue, std::string prefix)
    : venue_(venue), recorder_(std::move(prefix))
{
    // The venue's reports are tapped at the source, so what the file holds is what the venue said rather than
    // what a layer above this one chose to pass on. The callback the pipeline installs afterwards is called
    // second, which keeps the recording complete even if that callback is replaced mid-run.
    venue_.set_report_callback(
        [this](const ExecutionReport& report)
        {
            recorder_.record_report(report);
            if (callback_) callback_(report);
        });
}

void RecordingEngine::send_order(const exchange::Order& order)
{
    recorder_.record_submission(order);
    venue_.send_order(order);
}

void RecordingEngine::cancel_order(uint64_t order_id)
{
    recorder_.record_cancel(order_id);
    venue_.cancel_order(order_id);
}

void RecordingEngine::set_report_callback(ReportCallback cb)
{
    callback_ = std::move(cb);
}

RecordedVenue::RecordedVenue(std::string path) : path_(std::move(path)), rows_(read_order_log(path_))
{
    counters_.rows_left = rows_.size();
}

void RecordedVenue::send_order(const exchange::Order& order)
{
    if (cursor_ >= rows_.size())
        throw std::runtime_error(path_ + ": the recording ended and the replay still asked for order " +
                                 std::to_string(order.id) +
                                 ", so the recorded session and this one are not the same length");
    const OrderLogRow& row = rows_[cursor_];
    if (row.kind != OrderLogKind::Submission || row.order_id != order.id || row.symbol != order.symbol ||
        row.side != order.side || row.price != order.price || row.quantity != order.qty)
    {
        mismatch(row, "place order " + std::to_string(order.id) + " " + order.symbol + " " +
                          side_text(order.side) + " " + std::to_string(order.price) + " x" +
                          std::to_string(order.qty));
    }
    ++counters_.submissions;
    queue_reports_after(cursor_);
}

void RecordedVenue::cancel_order(uint64_t order_id)
{
    if (cursor_ >= rows_.size())
        throw std::runtime_error(path_ + ": the recording ended and the replay still asked to cancel order " +
                                 std::to_string(order_id));
    const OrderLogRow& row = rows_[cursor_];
    if (row.kind != OrderLogKind::Cancel || row.order_id != order_id)
        mismatch(row, "cancel order " + std::to_string(order_id));
    ++counters_.cancels;
    queue_reports_after(cursor_);
}

void RecordedVenue::queue_reports_after(std::size_t index)
{
    std::size_t next = index + 1;
    while (next < rows_.size() && rows_[next].kind == OrderLogKind::Report)
    {
        const OrderLogRow& row = rows_[next];
        ExecutionReport report;
        report.order_id = row.order_id;
        report.symbol = row.symbol;
        report.side = row.side;
        report.price = row.price;
        report.quantity = row.quantity;
        report.ts = row.ts;
        report.exec_type = row.exec_type;
        report.cancel_reason = row.cancel_reason;
        report.reject_reason = row.reject_reason;
        report.liquidity_role = row.liquidity_role;
        report.fee = row.fee;
        report.owner = row.owner;
        // Held rather than delivered: the report's own timestamp is the clock it is due on, and the run that reads
        // it is the run whose clock has to reach it first.
        pending_.emplace_back(row.ts, report);
        ++next;
    }
    cursor_ = next;
    counters_.reports_pending = pending_.size();
    counters_.rows_left = rows_.size() - cursor_ + pending_.size();
}

void RecordedVenue::advance_to(uint64_t now_us)
{
    // Strictly past, not at: a report was booked *during* the event at its timestamp, after the strategy had already
    // decided at that event, so the first decision that saw it was the next event's. Releasing it at the same
    // timestamp would hand the strategy a fill one event early, and the order sequence diverges from there.
    while (!pending_.empty() && pending_.front().first < now_us)
    {
        emit(pending_.front().second);
        pending_.pop_front();
    }
    counters_.reports_pending = pending_.size();
    counters_.rows_left = rows_.size() - cursor_ + pending_.size();
}

void RecordedVenue::flush()
{
    while (!pending_.empty())
    {
        ++counters_.flushed_at_end;
        emit(pending_.front().second);
        pending_.pop_front();
    }
    counters_.reports_pending = 0;
    counters_.rows_left = rows_.size() - cursor_;
}

void RecordedVenue::emit(const ExecutionReport& report)
{
    ++counters_.reports;
    if (callback_) callback_(report);
}

void RecordedVenue::mismatch(const OrderLogRow& row, const std::string& asked) const
{
    std::string expected;
    switch (row.kind)
    {
        case OrderLogKind::Submission:
            expected = "order " + std::to_string(row.order_id) + " " + row.symbol + " " +
                       side_text(row.side) + " " + std::to_string(row.price) + " x" +
                       std::to_string(row.quantity);
            break;
        case OrderLogKind::Cancel:
            expected = "a cancel for order " + std::to_string(row.order_id);
            break;
        case OrderLogKind::Report:
            expected = "a report for order " + std::to_string(row.order_id);
            break;
    }
    throw std::runtime_error(path_ + ":" + std::to_string(row.line) + ": the replay asked to " + asked +
                             " and the recording has " + expected);
}
