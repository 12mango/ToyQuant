# 8. Writing your own strategy

*Part of the guide. The overview is [`USER_GUIDE.md`](../USER_GUIDE.md); the strategy layer's design is
[the architecture](../architecture/05-strategy.md).*

This page is the injection point: which class to implement, how a name reaches the binary, and what the gate will
insist on before your strategy counts as part of the project. There are two seams, and picking the wrong one is the usual
first mistake.

## The interface

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
the real id and submits. The header carries the reason for each default, and the reasons are worth reading: they are
where a design decision about *who owns a number* is written down.

## Which seam

| you are writing | derive from | override |
|---|---|---|
| a **quoter** -- it rests orders and earns the spread | `L2MarketMakerBase` (`src/strategy/l2_market_maker_base.h`) | `fair_price()`, or `quote()` if the quoting rule itself changes |
| a **position** strategy -- a signal sets a target instead of a quote | `Strategy` directly, as `trend_l2` and `execution_l2` do | `on_l2_market_view()` (or the trade hook) and the six required methods |
| a **comparison** of existing signals | nothing: the active line is one class under several `L2SignalMode` configs, so a new signal is a `case` in `fair_price()`, not a class | -- |

The third row is the one the repository already uses eight times: `passive_l2`, `l2_depth`, `l2_micro`, `l2_flow`,

## The four steps a new strategy takes

1. **Implement it.** A quoter only needs the fair price; the base class keeps the inventory limit, the gates, the requote
   rule and the flatten band.

   ```cpp
   class MyMaker final : public L2MarketMakerBase
   {
     public:
       explicit MyMaker(const L2MarketMakerConfig& config) : L2MarketMakerBase(config) {}

     private:
       double fair_price(const L2MarketView& view) const override
       {
           // Your idea, in ticks off the mid. Everything downstream is the repository's.
           return view.mid + clock_edge_ticks(view);
       }
   };
   ```

2. **Register the name.** Two places, and the second has a trap: `make_strategy()` in `src/app/strategy_factory.cpp` gets a
   branch, and the name goes into `kStrategyNames` in `src/app/app_config.cpp`. That array is declared
   `std::array<std::string_view, 19>`, so **adding a name means editing the count as well**; the usage text prints the
   array, which is why a name cannot be accepted and undocumented at the same time.

3. **Run it.** The strategy name is the sixth positional argument of `l2_replay`:

   ```bash
   ./out/build/linux-debug/toy_quant l2_replay data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz \
       /tmp/depth_200k_base.csv BTC-PERPETUAL 0 my_maker 1 prorata --fast-validation
   ```

   `/tmp/depth_200k_base.csv` is cut by `bash tools/gate/verify_l2.sh smoke` if it is not already there.

4. **Prove nothing else moved.** `bash tools/gate/verify_l2.sh code` runs the invariants and the factory test; `fast` adds
   every number the documents quote. Adding a class must not move a single recorded figure -- if it does, the change was
   in the shared base rather than in your strategy, and the diff should say so.

## What a strategy is not allowed to do

The header argues each of these, and they are what keeps a new strategy from becoming a second source of truth:

- **It cannot measure its own execution.** Capture, markout, quote lifetime and inventory are computed once by the
  pipeline, from the reports and the events, because a strategy's own copy can only disagree with it -- and did, printing
  `captured_edge=0` beside an `[EXECUTION]` line that said 1.25 in the same run.
- **It cannot see the midpoint after a fill.** The markout is pushed to it through `on_markout_estimate`, which is the
  only way a strategy can learn what its own passive fills cost.
- **It cannot keep its own position.** The ledger owns it; `net_position()` reports, and the pipeline reconciles.
- **It cannot own the clock or the fee model.** The pipeline advances the event clock and the engine charges the schedule.
- **It cannot bypass a pre-trade control.** A refusal arrives as `on_order_rejected` with the reason, and the counter stays
  with the pipeline.

## What the gate will ask of the result

A new strategy is a new line, and the project's own sentence about lines applies: **a window selected for being positive
is not an edge.** The gate asks the cheap questions -- invariants, the factory test, and that no recorded number moved --
and the documents ask the expensive one: is this out of sample, over how many windows, and does it survive its own
decomposition (capture less markout less fees). The tools for that are in [the tools](../TOOLS.md), and the terms are
defined in [the glossary](../GLOSSARY.md).

`inventory_aware_l2` and `active_l2` are three classes over one signal enum, which is why the names outnumber the classes.
