#pragma once
#include <cstdint>
#include <string>
#include <string_view>

#include "exchange/execution_report.h"
#include "exchange/matching_engine.h"
#include "exchange/order.h"

// The wire a venue is reached over. This is the seam ADR 0005 turns on: the adapter below is written
// against this interface, so the shape of "an order goes out and a report comes back" is proved by a test
// with a recorder in place of a socket, before any socket exists.
class IVenueTransport
{
  public:
    virtual ~IVenueTransport() = default;

    // False when the frame could not be written. The adapter counts that and does not pretend the order was
    // sent: a silent success on a dead wire is the one failure a demonstration cannot afford.
    virtual bool send(std::string_view frame) = 0;
};

struct LiveVenueConfig
{
    // The adapter's own bound, in orders. The pre-trade pipeline already caps notional, rate and size, and
    // those protect against a strategy's behaviour; this one protects against the adapter and the strategy
    // disagreeing about what was sent. 0 disables it, which is the configuration a test uses.
    uint64_t max_orders{0};
    // The same bound on one order's size, in the instrument's own quantity units. 0 disables it.
    uint64_t max_order_quantity{0};
};

struct LiveVenueCounters
{
    uint64_t orders_sent{0};
    uint64_t cancels_sent{0};
    // Refused by this adapter rather than by the pipeline: a cap, or a transport that would not take it.
    uint64_t refused_by_adapter{0};
    uint64_t transport_failures{0};
    uint64_t reports_decoded{0};
    // A frame the decoder did not understand. Counted rather than ignored, because a venue's new field must
    // not look like a venue that said nothing.
    uint64_t frames_dropped{0};
};

// A venue on the other end of a wire. The same decorator shape as `PaperBroker` and `OrderGateway`: the
// pipeline knows only `IMatchingEngine`, so this class is the whole change, and it lives in `src/lines/`
// because it is the layer allowed to know a venue's shapes.
//
// The frames below are this adapter's own protocol, not a venue's JSON. Translating to a venue's message
// shapes is the relay's job -- the layer that already knows them -- so step two of ADR 0005 replaces the
// transport and leaves this file alone.
//
//     out:  ORDER <id> <symbol> <B|S> <price> <qty> <Gtc|Ioc|Fok>      CANCEL <id>
//     in:   ACK <id> <remaining>   FILL <id> <price> <qty>
//           CANCELED <id> <reason>  REJECT <id> <reason>
class LiveVenue : public IMatchingEngine
{
  public:
    explicit LiveVenue(IVenueTransport& transport, LiveVenueConfig config = {})
        : transport_(transport), config_(config)
    {
    }

    void send_order(const exchange::Order& order) override;
    void cancel_order(uint64_t order_id) override;

    // A live venue matches its own orders, so the two calls the in-process engine needs to simulate fills do
    // nothing here. That is the one place this interface mixes "be a market" with "be a venue", and ADR 0005
    // records it as the smell to watch rather than as a reason to widen the interface today.
    void process_bbo(const BboQuote& quote) override { (void)quote; }
    void process_market_trade(const MarketTrade& trade) override { (void)trade; }

    void set_report_callback(ReportCallback cb) override { callback_ = std::move(cb); }

    // One frame from the venue. Returns false when it was not understood, so the caller can count it.
    bool on_frame(std::string_view frame);

    const LiveVenueCounters& counters() const { return counters_; }

  private:
    IVenueTransport& transport_;
    LiveVenueConfig config_;
    LiveVenueCounters counters_;
    ReportCallback callback_;
};
