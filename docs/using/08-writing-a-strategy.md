# 8. Writing your own strategy

*Part of the guide. The overview is [`USER_GUIDE.md`](../USER_GUIDE.md); the strategy layer's design is
[the architecture](../architecture/05-strategy.md).*

One class is the injection point. A quoter supplies a fair price and gets two resting orders from the base; a position
strategy overrides the book hook and returns its own orders. Everything below is the smallest version that compiles.

```cpp
class MyMaker final : public L2MarketMakerBase
{
  public:
    explicit MyMaker(const L2MarketMakerConfig& config) : L2MarketMakerBase(config) {}

  private:
    // The book update: the only method a quoter has to answer.
    double fair_price(const L2MarketView& view) const override
    {
        return view.mid + edge_ticks(view);          // your idea, in ticks
    }

    // The report: the only place a fill reaches a strategy.
    void on_order_update(const ExecutionReport& report) override
    {
        L2MarketMakerBase::on_order_update(report);  // base keeps inventory and open orders
        // your own signal state goes here; net_position() is the base's, not a copy
    }
};
```

## Three red lines

The strategy runs inside the event loop, once per book update, and the loop's budget is what makes a latency number mean
anything:

- **no allocation of its own** -- the order vector a decision returns is the interface's one allocation, and a strategy
  that adds another per event is measuring the allocator;
- **no blocking I/O and no lock** -- the consumer is single-threaded by construction, so a mutex or a syscall in a
  callback stops the clock the run is measuring;
- **no wall clock** -- time is the event clock (`L2MarketView`'s timestamp), and `steady_clock` in a strategy makes the
  run non-reproducible, which is the one thing the gate cannot catch for you.

/// details | The full interface, the seams, and the four steps a new strategy takes

Everything a strategy can be is in `src/strategy/strategy.h`, one abstract class:

```cpp
class Strategy
{
  public:
    // The six a subclass has to answer.
    virtual std::vector<StrategyOrder> on_top_of_book(const std::string& symbol, const TopOfBook& tob) = 0;
    virtual void on_order_submitted(const StrategyOrder& order) = 0;
    virtual std::vector<uint64_t> cancel_requests() = 0;
    virtual int64_t net_position() const = 0;
    virtual size_t working_order_count() const = 0;
    virtual void on_order_update(const ExecutionReport& rpt) = 0;

    // The hooks that have a default, so a strategy pays only for what it uses.
    virtual std::vector<StrategyOrder> on_l2_market_view(const L2MarketView& view);  // defaults to on_top_of_book
    virtual void on_market_trade(const MarketTrade& trade, const BboQuote* latest_bbo);
    virtual void on_queue_activity(Side side, uint64_t consumed_quantity);
    virtual void on_order_rejected(const StrategyOrder& order, std::string_view reason);
    virtual void on_markout_estimate(const MarkoutEstimate& estimate);
    virtual StrategyMetrics metrics() const;
};
```

`StrategyOrder` carries side, symbol, price, quantity, an id, a time in force and a `post_only` flag; the pipeline assigns
the real id and submits. The header carries the reason for each default, and those reasons are where a design decision
about *who owns a number* is written down.

| you are writing | derive from | override |
|---|---|---|
| a **quoter** -- it rests orders and earns the spread | `L2MarketMakerBase` | `fair_price()`, or `quote()` if the rule itself changes |
| a **position** strategy -- a signal sets a target | `Strategy` directly, as `trend_l2` and `execution_l2` do | `on_l2_market_view()` and the six required methods |
| a **comparison** of existing signals | nothing: a new signal is a `case` in `fair_price()` | -- |

1. **Implement it.** A quoter only needs the fair price; the base keeps the inventory limit, the gates, the requote rule
   and the flatten band.
2. **Register the name.** A branch in `make_strategy()` (`src/app/strategy_factory.cpp`) and the name in `kStrategyNames`
   (`src/app/app_config.cpp`), whose `std::array` count has to be edited with it; the usage text prints the array, so a
   name cannot be accepted and undocumented at once.
3. **Run it.** The strategy name is the sixth positional of `l2_replay`; `/tmp/depth_200k_base.csv` is cut by
   `bash tools/gate/verify_l2.sh smoke`.
4. **Prove nothing else moved.** `bash tools/gate/verify_l2.sh code` runs the invariants and the factory test, and `fast`
   adds every quoted number. Adding a class must not move a single recorded figure; if it does, the change was in the
   shared base, and the diff should say so.

///

## What a strategy is not allowed to do

The header argues each of these, and they are what keeps a new strategy from becoming a second source of truth: it cannot
measure its own execution (capture, markout, lifetime and inventory are computed once by the pipeline, from the reports
and the events), it cannot see the midpoint after a fill except through `on_markout_estimate`, it cannot keep its own
position (`net_position()` reports; the pipeline reconciles), it cannot own the clock or the fee model, and it cannot
bypass a pre-trade control -- a refusal arrives as `on_order_rejected` and the counter stays with the pipeline.

A new strategy is a new line, and the project's own sentence applies: **a window selected for being positive is not an
edge.** The gate asks the cheap questions -- invariants, the factory test, and that no recorded number moved -- and the
documents ask the expensive one: is it out of sample, over how many windows, and does it survive its own decomposition
(capture less markout less fees). The tools are in [the tools](../TOOLS.md) and the terms in [the glossary](../GLOSSARY.md).

`inventory_aware_l2` and `active_l2` are three classes over one signal enum, which is why the names outnumber the classes.
