#include <cassert>
#include <cmath>

#include "orderbook/l2_orderbook.h"
#include "strategy/active_l2_market_maker.h"
#include "strategy/legacy/inventory_aware_l2_market_maker.h"
#include "strategy/l2_market_maker_base.h"
#include "strategy/trend_l2.h"

int main()
{
    L2OrderBook book;
    book.apply_snapshot(MarketDepthSnapshot{.ts = 100,
                                            .symbol = "BTC-PERPETUAL",
                                            .sequence = 1,
                                            .bids = {{6421.0, 141360}, {6420.5, 100}},
                                            .asks = {{6421.5, 18640}, {6422.0, 200}},
                                            .exchange = "deribit"});

    const L2MarketView view = book.market_view();
    assert(std::abs(view.top.bid_price - 6421.0) < 1e-9);
    assert(std::abs(view.top.ask_price - 6421.5) < 1e-9);
    assert(view.depth_imbalance > 0.0);
    assert(view.micro_price > view.top.bid_price);

    L2MarketMakerBase strategy(L2MarketMakerBaseConfig{.order_size = 1,
                                                .base_spread = 1.0,
                                                .inventory_limit = 100,
                                                .tick_size = 0.5,
                                                .imbalance_shift = 1.0});
    const auto orders = strategy.on_l2_market_view(view);
    assert(orders.size() == 2);
    assert(orders[0].side == Side::Buy);
    assert(orders[1].side == Side::Sell);
    assert(orders[0].price < orders[1].price);

    L2MarketMakerBase baseline(L2MarketMakerBaseConfig{.order_size = 1,
                                                .base_spread = 1.0,
                                                .inventory_limit = 100,
                                                .tick_size = 0.5,
                                                .signal_mode = L2SignalMode::Baseline});
    const auto baseline_orders = baseline.on_l2_market_view(view);
    assert(baseline_orders.size() == 2);
    assert(baseline_orders[0].price < orders[0].price);

    L2MarketMakerBase passive_guard(L2MarketMakerBaseConfig{.order_size = 1,
                                                     .base_spread = 0.5,
                                                     .inventory_limit = 100,
                                                     .tick_size = 0.5,
                                                     .imbalance_shift = 10.0,
                                                     .signal_mode = L2SignalMode::Depth});
    const auto guarded_orders = passive_guard.on_l2_market_view(view);
    assert(guarded_orders.size() == 2);
    assert(guarded_orders[0].price <= view.top.bid_price);
    assert(guarded_orders[1].price >= view.top.ask_price);

    L2MarketMakerBase refreshable(L2MarketMakerBaseConfig{.order_size = 1,
                                                   .base_spread = 1.0,
                                                   .inventory_limit = 100,
                                                   .tick_size = 0.5,
                                                   .refresh_price_ticks = 2,
                                                   .max_quote_age = 10});
    const auto initial_orders = refreshable.on_l2_market_view(view);
    assert(initial_orders.size() == 2);
    refreshable.on_order_submitted(StrategyOrder{Side::Buy, view.symbol.str(), initial_orders[0].price, 1,
                                                 1});
    refreshable.on_order_submitted(StrategyOrder{Side::Sell, view.symbol.str(), initial_orders[1].price, 1,
                                                 2});
    assert(refreshable.on_l2_market_view(view).empty());
    assert(refreshable.cancel_requests().empty());

    L2MarketView moved_view = view;
    moved_view.top.bid_price += 2.0;
    moved_view.top.ask_price += 2.0;
    assert(refreshable.on_l2_market_view(moved_view).empty());
    assert(refreshable.cancel_requests().size() == 2);

    L2MarketMakerBase depth_only_updates(L2MarketMakerBaseConfig{.order_size = 1,
                                                         .base_spread = 1.0,
                                                         .inventory_limit = 100,
                                                         .tick_size = 0.5,
                                                         .signal_mode = L2SignalMode::Baseline,
                                                         .max_quote_age = 2});
    const auto depth_only_orders = depth_only_updates.on_l2_market_view(view);
    assert(depth_only_orders.size() == 2);
    depth_only_updates.on_order_submitted(
        StrategyOrder{Side::Buy, view.symbol.str(), depth_only_orders[0].price, 1, 33});
    depth_only_updates.on_order_submitted(
        StrategyOrder{Side::Sell, view.symbol.str(), depth_only_orders[1].price, 1, 34});
    L2MarketView same_top = view;
    same_top.depth_imbalance = -view.depth_imbalance;
    assert(depth_only_updates.on_l2_market_view(same_top).empty());
    assert(depth_only_updates.cancel_requests().empty());

    L2MarketMakerBase queue_patient(L2MarketMakerBaseConfig{.order_size = 1,
                                                     .base_spread = 1.0,
                                                     .inventory_limit = 100,
                                                     .tick_size = 0.5,
                                                     .signal_mode = L2SignalMode::Baseline,
                                                     .refresh_price_ticks = 2,
                                                     .max_quote_age = 1});
    const auto queue_orders = queue_patient.on_l2_market_view(view);
    assert(queue_orders.size() == 2);
    queue_patient.on_order_submitted(
        StrategyOrder{Side::Buy, view.symbol.str(), queue_orders[0].price, 1, 51});
    queue_patient.on_order_submitted(
        StrategyOrder{Side::Sell, view.symbol.str(), queue_orders[1].price, 1, 52});
    queue_patient.on_queue_activity(Side::Buy, 100);
    L2MarketView queue_move = view;
    queue_move.top.bid_price += 0.5;
    queue_move.top.ask_price += 0.5;
    assert(queue_patient.on_l2_market_view(queue_move).empty());
    assert(queue_patient.cancel_requests().empty());
    for (int hold = 0; hold < 2; ++hold)
    {
        queue_patient.on_queue_activity(Side::Buy, 100);
        assert(queue_patient.on_l2_market_view(queue_move).empty());
        assert(queue_patient.cancel_requests().empty());
    }
    queue_patient.on_queue_activity(Side::Buy, 100);
    assert(queue_patient.on_l2_market_view(queue_move).empty());
    assert(queue_patient.cancel_requests().size() == 2);

    L2MarketMakerBase guarded_buy_flow(L2MarketMakerBaseConfig{.order_size = 1,
                                                        .base_spread = 1.0,
                                                        .inventory_limit = 100,
                                                        .tick_size = 0.5,
                                                        .signal_mode = L2SignalMode::Flow,
                                                        .flow_guard = true,
                                                        .toxicity_flow_threshold = 0.5});
    guarded_buy_flow.on_market_trade(MarketTrade{99, view.symbol.str(), 6421.5, 100, Side::Buy, 1,
                                                 "deribit"});
    const auto buy_flow_orders = guarded_buy_flow.on_l2_market_view(view);
    assert(buy_flow_orders.size() == 1);
    assert(buy_flow_orders[0].side == Side::Buy);

    L2MarketMakerBase guarded_sell_flow(L2MarketMakerBaseConfig{.order_size = 1,
                                                         .base_spread = 1.0,
                                                         .inventory_limit = 100,
                                                         .tick_size = 0.5,
                                                         .signal_mode = L2SignalMode::Flow,
                                                        .flow_guard = true,
                                                         .toxicity_flow_threshold = 0.5});
    guarded_sell_flow.on_market_trade(MarketTrade{99, view.symbol.str(), 6421.0, 100, Side::Sell, 1,
                                                  "deribit"});
    const auto sell_flow_orders = guarded_sell_flow.on_l2_market_view(view);
    assert(sell_flow_orders.size() == 1);
    assert(sell_flow_orders[0].side == Side::Sell);

    L2MarketMakerBase short_flow(L2MarketMakerBaseConfig{.order_size = 1,
                                                  .base_spread = 1.0,
                                                  .inventory_limit = 100,
                                                  .tick_size = 0.5,
                                                  .signal_mode = L2SignalMode::Flow,
                                                        .flow_guard = true,
                                                  .trade_window = 1,
                                                  .toxicity_flow_threshold = 0.5});
    short_flow.on_market_trade(MarketTrade{99, view.symbol.str(), 6421.5, 100, Side::Buy, 1,
                                           "deribit"});
    short_flow.on_market_trade(MarketTrade{100, view.symbol.str(), 6421.0, 100, Side::Sell, 2,
                                           "deribit"});
    const auto latest_flow_orders = short_flow.on_l2_market_view(view);
    assert(latest_flow_orders.size() == 1);
    assert(latest_flow_orders[0].side == Side::Sell);

    InventoryAwareL2MarketMaker inventory_strategy(
        InventoryAwareL2MarketMakerConfig{
            .base = L2MarketMakerBaseConfig{.order_size = 2,
                                         .base_spread = 1.0,
                                         .inventory_limit = 2,
                                         .tick_size = 0.5,
                                         .signal_mode = L2SignalMode::Flow,
                                                        .flow_guard = true,
                                         .toxicity_flow_threshold = 0.65},
            .inventory_limit = 2,
            .max_inventory_shift_ticks = 2.0});
    const auto initial_inventory_orders = inventory_strategy.on_l2_market_view(view);
    assert(initial_inventory_orders.size() == 2);
    inventory_strategy.on_order_submitted(
        StrategyOrder{Side::Buy, view.symbol.str(), initial_inventory_orders[0].price, 2, 10});
    inventory_strategy.on_order_update(ExecutionReport{.order_id = 10,
                                                        .side = exchange::Side::Buy,
                                                        .exec_type = ExecType::Trade,
                                                        .symbol = view.symbol.str(),
                                                        .price = initial_inventory_orders[0].price,
                                                        .quantity = 2,
                                                        .owner = "MarketMaker"});
    const auto long_inventory_orders = inventory_strategy.on_l2_market_view(view);
    assert(long_inventory_orders.size() == 1);
    assert(long_inventory_orders[0].side == Side::Sell);

    ActiveL2MarketMaker active(ActiveL2MarketMakerConfig{
        .base = L2MarketMakerBaseConfig{.order_size = 2,
                                     .base_spread = 1.0,
                                     .inventory_limit = 100,
                                     .tick_size = 0.5,
                                     .signal_mode = L2SignalMode::Flow,
                                                        .flow_guard = true,
                                     .toxicity_flow_threshold = 0.65},
        .volatility_alpha = 0.5,
        .pause_after_ticks_per_second = 8.0});
    assert(active.on_l2_market_view(view).size() == 2);
    L2MarketView volatile_view = view;
    assert(active.on_l2_market_view(view).size() == 2);
    bool paused = false;
    for (int index = 0; index < 10; ++index)
    {
        volatile_view.top.bid_price += index % 2 == 0 ? 10.0 : -10.0;
        volatile_view.top.ask_price += index % 2 == 0 ? 10.0 : -10.0;
        if (active.on_l2_market_view(volatile_view).empty()) paused = true;
    }
    assert(paused);
    assert(active.volatility_ticks() >= 8.0);

    ActiveL2MarketMaker warming(ActiveL2MarketMakerConfig{
        .base = L2MarketMakerBaseConfig{.order_size = 1,
                                     .base_spread = 1.0,
                                     .inventory_limit = 100,
                                     .tick_size = 0.5,
                                     .signal_mode = L2SignalMode::Flow,
                                                        .flow_guard = true,
                                     .toxicity_flow_threshold = 0.65},
        .warmup_trades = 2,
        .warmup_views = 2});
    assert(warming.on_l2_market_view(view).empty());
    warming.on_market_trade(MarketTrade{101, view.symbol.str(), 6421.5, 1, Side::Buy, 1, "test"});
    warming.on_market_trade(MarketTrade{102, view.symbol.str(), 6421.5, 1, Side::Sell, 2, "test"});
    assert(warming.on_l2_market_view(view).size() == 2);

    ActiveL2MarketMaker time_aware(ActiveL2MarketMakerConfig{
        .base = L2MarketMakerBaseConfig{.order_size = 2,
                                     .base_spread = 1.0,
                                     .inventory_limit = 100,
                                     .tick_size = 0.5,
                                     .signal_mode = L2SignalMode::Flow,
                                                        .flow_guard = true,
                                     .toxicity_flow_threshold = 0.65},
        .volatility_alpha = 0.5,
                        .pause_after_ticks_per_second = 8.0});
    L2MarketView base_time_view = view;
    base_time_view.ts = 1000;
    time_aware.on_l2_market_view(base_time_view);
    L2MarketView slow_move = base_time_view;
    slow_move.ts = 2000000;
    slow_move.top.bid_price += 10.0;
    slow_move.top.ask_price += 10.0;
    time_aware.on_l2_market_view(slow_move);
    const double slow_volatility = time_aware.volatility_ticks();

    ActiveL2MarketMaker fast_time_aware(ActiveL2MarketMakerConfig{
        .base = L2MarketMakerBaseConfig{.order_size = 2,
                                     .base_spread = 1.0,
                                     .inventory_limit = 100,
                                     .tick_size = 0.5,
                                     .signal_mode = L2SignalMode::Flow,
                                                        .flow_guard = true,
                                     .toxicity_flow_threshold = 0.65},
        .volatility_alpha = 0.5,
                        .pause_after_ticks_per_second = 8.0});
    fast_time_aware.on_l2_market_view(base_time_view);
    L2MarketView fast_move = base_time_view;
    fast_move.ts = 1001;
    fast_move.top.bid_price += 10.0;
    fast_move.top.ask_price += 10.0;
    fast_time_aware.on_l2_market_view(fast_move);
    const double fast_volatility = fast_time_aware.volatility_ticks();
    assert(slow_volatility > 0.0);
    assert(fast_volatility > slow_volatility);

    ActiveL2MarketMaker pausing(ActiveL2MarketMakerConfig{
        .base = L2MarketMakerBaseConfig{.order_size = 2,
                                     .base_spread = 1.0,
                                     .inventory_limit = 100,
                                     .tick_size = 0.5,
                                     .signal_mode = L2SignalMode::Flow,
                                                        .flow_guard = true,
                                     .toxicity_flow_threshold = 0.65},
        .volatility_alpha = 1.0,
                        .pause_after_ticks_per_second = 5.0});
    L2MarketView calm_view = view;
    calm_view.ts = 1000000;
    const auto pausing_orders = pausing.on_l2_market_view(calm_view);
    assert(pausing_orders.size() == 2);
    pausing.on_order_submitted(
        StrategyOrder{Side::Buy, view.symbol.str(), pausing_orders[0].price, 2, 21});
    pausing.on_order_submitted(
        StrategyOrder{Side::Sell, view.symbol.str(), pausing_orders[1].price, 2, 22});
    L2MarketView shock_view = calm_view;
    shock_view.ts += 1000000;
    shock_view.top.bid_price += 5.0;
    shock_view.top.ask_price += 5.0;
    assert(pausing.on_l2_market_view(shock_view).empty());
    assert(pausing.cancel_requests().size() == 2);

    // The forced-flow cooldown: a mid that moves two ticks inside 40 ms pulls the quotes and holds them off for
    // 75 ms. The speed gate is pushed out of reach, so only the cooldown can be responsible for a pause.
    ActiveL2MarketMaker cooldown(ActiveL2MarketMakerConfig{
        .base = L2MarketMakerBaseConfig{.order_size = 2,
                                     .base_spread = 1.0,
                                     .inventory_limit = 100,
                                     .tick_size = 0.5,
                                     .signal_mode = L2SignalMode::Flow,
                                     .flow_guard = true,
                                     .toxicity_flow_threshold = 0.65},
        .volatility_alpha = 0.0,
        .pause_after_ticks_per_second = 1.0e9,
        .jump_window_ms = 40,
        .jump_ticks = 2.0,
        .jump_cooldown_ms = 75});
    L2MarketView calm_quote = view;
    calm_quote.ts = 1000000;
    const auto cooldown_orders = cooldown.on_l2_market_view(calm_quote);
    assert(cooldown_orders.size() == 2);
    cooldown.on_order_submitted(
        StrategyOrder{Side::Buy, view.symbol.str(), cooldown_orders[0].price, 2, 61});
    cooldown.on_order_submitted(
        StrategyOrder{Side::Sell, view.symbol.str(), cooldown_orders[1].price, 2, 62});
    L2MarketView jump_view = calm_quote;
    jump_view.ts += 10000;
    jump_view.top.bid_price += 1.0;  // two ticks in ten milliseconds is a jump
    jump_view.top.ask_price += 1.0;
    assert(cooldown.on_l2_market_view(jump_view).empty());
    assert(cooldown.cancel_requests().size() == 2);
    cooldown.on_order_update(ExecutionReport{.order_id = 61, .side = exchange::Side::Buy,
                                             .exec_type = ExecType::Cancelled,
                                             .symbol = view.symbol.str()});
    cooldown.on_order_update(ExecutionReport{.order_id = 62, .side = exchange::Side::Sell,
                                             .exec_type = ExecType::Cancelled,
                                             .symbol = view.symbol.str()});
    L2MarketView inside_cooldown = jump_view;
    inside_cooldown.ts += 20000;  // still inside the 75 ms it was armed for
    assert(cooldown.on_l2_market_view(inside_cooldown).empty());
    L2MarketView past_cooldown = inside_cooldown;
    past_cooldown.ts += 80000;
    assert(cooldown.on_l2_market_view(past_cooldown).size() == 2);

    // One tick inside the same window is a drift, not a jump, so quoting continues.
    ActiveL2MarketMaker drift(ActiveL2MarketMakerConfig{
        .base = L2MarketMakerBaseConfig{.order_size = 2,
                                     .base_spread = 1.0,
                                     .inventory_limit = 100,
                                     .tick_size = 0.5,
                                     .signal_mode = L2SignalMode::Flow,
                                     .flow_guard = true,
                                     .toxicity_flow_threshold = 0.65},
        .volatility_alpha = 0.0,
        .pause_after_ticks_per_second = 1.0e9,
        .jump_window_ms = 40,
        .jump_ticks = 2.0,
        .jump_cooldown_ms = 75});
    assert(drift.on_l2_market_view(calm_quote).size() == 2);
    L2MarketView small_move = calm_quote;
    small_move.ts += 10000;
    small_move.top.bid_price += 0.5;
    small_move.top.ask_price += 0.5;
    assert(drift.on_l2_market_view(small_move).size() == 2);

    // The asymmetric refresh: a requote that would move the bid *up* while the fair is still moving is held back,
    // and the ask that retreats goes out at once; once the fair has been still for the debounce, the bid follows.
    L2MarketMakerBase asymmetric(L2MarketMakerBaseConfig{.order_size = 1,
                                                        .base_spread = 1.0,
                                                        .inventory_limit = 100,
                                                        .tick_size = 0.5,
                                                        .advance_debounce_ms = 40});
    L2MarketView symmetric_start = view;
    symmetric_start.ts = 2000000;
    const auto asymmetric_orders = asymmetric.on_l2_market_view(symmetric_start);
    assert(asymmetric_orders.size() == 2);
    asymmetric.on_order_submitted(
        StrategyOrder{Side::Buy, view.symbol.str(), asymmetric_orders[0].price, 1, 71});
    asymmetric.on_order_submitted(
        StrategyOrder{Side::Sell, view.symbol.str(), asymmetric_orders[1].price, 1, 72});
    L2MarketView rising = symmetric_start;
    rising.ts += 1000;
    rising.top.bid_price += 1.0;
    rising.top.ask_price += 1.0;
    // The refresh arms the cancel, and the reports that answer it arrive before the requote.
    assert(asymmetric.on_l2_market_view(rising).empty());
    assert(asymmetric.cancel_requests().size() == 2);
    asymmetric.on_order_update(ExecutionReport{.order_id = 71, .side = exchange::Side::Buy,
                                               .exec_type = ExecType::Cancelled,
                                               .symbol = view.symbol.str()});
    asymmetric.on_order_update(ExecutionReport{.order_id = 72, .side = exchange::Side::Sell,
                                               .exec_type = ExecType::Cancelled,
                                               .symbol = view.symbol.str()});
    // One millisecond after the fair moved: the ask retreats and goes out, the bid would chase and waits.
    const auto deflected = asymmetric.on_l2_market_view(rising);
    assert(deflected.size() == 1);
    assert(deflected[0].side == Side::Sell);
    // The same prices once the fair has been still for the debounce: both sides, and the bid has caught up.
    L2MarketView settled = rising;
    settled.ts += 60000;
    const auto caught_up = asymmetric.on_l2_market_view(settled);
    assert(caught_up.size() == 2);
    assert(caught_up[0].side == Side::Buy);
    assert(caught_up[0].price > asymmetric_orders[0].price);

    // The closing side comes in while there is inventory to close: a long position is reduced by the ask, so the ask
    // is quoted closer to the fair while the bid keeps its own distance.
    // The limit is huge so the existing inventory shift is negligible: this test is about the closing side's distance,
    // not about the shift, which moves both quotes and would confound the comparison.
    L2MarketMakerBase closing(L2MarketMakerBaseConfig{.order_size = 1,
                                                      .base_spread = 1.5,
                                                      .inventory_limit = 1000000000,
                                                      .tick_size = 0.5,
                                                      .close_side_ticks = 1.0});
    L2MarketView flat_view = view;
    flat_view.ts = 3000000;
    const auto flat_orders = closing.on_l2_market_view(flat_view);
    assert(flat_orders.size() == 2);
    closing.on_order_submitted(
        StrategyOrder{Side::Buy, view.symbol.str(), flat_orders[0].price, 1, 81});
    closing.on_order_update(ExecutionReport{.order_id = 81, .side = exchange::Side::Buy,
                                            .exec_type = ExecType::Trade,
                                            .symbol = view.symbol.str(),
                                            .price = flat_orders[0].price, .quantity = 1});
    assert(closing.net_position() > 0);
    const auto closing_orders = closing.on_l2_market_view(flat_view);
    assert(closing_orders.size() == 2);
    // The adding side did not move and the closing side came in, which is the whole of the rule.
    assert(closing_orders[0].price == flat_orders[0].price);
    assert(closing_orders[1].price < flat_orders[1].price);
    assert(closing_orders[1].price > flat_view.top.ask_price);

    // Extra layers quote behind the primary one at the same size: the fill count a window carries is bounded by the
    // number of orders, so a second layer a side is what raises it without moving the first layer's price.
    L2MarketMakerBase layered(L2MarketMakerBaseConfig{.order_size = 1,
                                                      .base_spread = 1.0,
                                                      .inventory_limit = 100,
                                                      .tick_size = 0.5,
                                                      .extra_layers = 1});
    L2MarketView layered_view = view;
    layered_view.ts = 4000000;
    const auto layered_orders = layered.on_l2_market_view(layered_view);
    assert(layered_orders.size() == 4);
    assert(layered_orders[0].side == Side::Buy);
    assert(layered_orders[1].side == Side::Sell);
    assert(std::abs(layered_orders[2].price - (layered_orders[0].price - 0.5)) < 1e-9);
    assert(std::abs(layered_orders[3].price - (layered_orders[1].price + 0.5)) < 1e-9);
    assert(layered_orders[2].quantity == layered_orders[0].quantity);

    // Moving a primary quote in place: with the flag on, a refresh emits an amend for the side whose target moved and
    // leaves that order out of the cancels; with it off, the same view cancels both. The request is a request -- this
    // replay's engine has no amend, so the pipeline turns it into the cancel it replaced and no recorded number moves.
    {
        const auto drive_refresh = [&](bool amend) {
            L2MarketMakerBase strategy(amend ? L2MarketMakerBaseConfig{.order_size = 1,
                                                                      .base_spread = 1.0,
                                                                      .inventory_limit = 100,
                                                                      .tick_size = 0.5,
                                                                      .amend_instead_of_cancel = true}
                                            : L2MarketMakerBaseConfig{.order_size = 1,
                                                                      .base_spread = 1.0,
                                                                      .inventory_limit = 100,
                                                                      .tick_size = 0.5});
            const auto placed = strategy.on_l2_market_view(view);
            assert(placed.size() == 2);
            strategy.on_order_submitted(StrategyOrder{placed[0].side, view.symbol.str(), placed[0].price, 1, 30});
            strategy.on_order_submitted(StrategyOrder{placed[1].side, view.symbol.str(), placed[1].price, 1, 31});
            L2MarketView moved = view;
            moved.ts = 5000000;
            moved.top.bid_price += 1.0;
            moved.top.ask_price += 1.0;
            strategy.on_l2_market_view(moved);
            return std::pair{strategy.cancel_requests(), strategy.amend_requests()};
        };
        const auto plain = drive_refresh(false);
        assert(plain.first.size() == 2);
        assert(plain.second.empty());
        const auto amended = drive_refresh(true);
        assert(amended.first.empty());
        assert(amended.second.size() == 2);
        assert(std::abs(amended.second[0].price - amended.second[1].price) > 0.5);
    }

    // A layer's step in basis points: the default is one tick, which on a fine tick puts the second layer on almost the
    // first layer's price. At ten bps a layer the step here -- fair 6421.25, tick 0.5 -- is 6.42 of price, 12.8 ticks,
    // rounded outward to thirteen, so the second layer sits 6.5 behind the first instead of 0.5.
    L2MarketMakerBase bps_layered(L2MarketMakerBaseConfig{.order_size = 1,
                                                          .base_spread = 1.0,
                                                          .inventory_limit = 100,
                                                          .tick_size = 0.5,
                                                          .extra_layers = 1,
                                                          .layer_step_bps = 10.0});
    L2MarketView bps_layered_view = view;
    bps_layered_view.ts = 4500000;
    const auto bps_layered_orders = bps_layered.on_l2_market_view(bps_layered_view);
    assert(bps_layered_orders.size() == 4);
    assert(std::abs((bps_layered_orders[0].price - bps_layered_orders[2].price) - 6.5) < 1e-9);
    assert(std::abs((bps_layered_orders[3].price - bps_layered_orders[1].price) - 6.5) < 1e-9);

    // The distance as basis points rather than ticks: one tick is worth `tick / price` of the price, so a target of
    // `D` bps is `max(2, round(D / tick_bps))` ticks -- the same economic distance on every instrument, which a fixed
    // count is not (UP's tick is 5.2 bps and DNA's 7.3). The floor is two ticks, so a target worth less than one
    // tick is the tick-count form's two-tick quote exactly.
    {
        L2MarketMakerBase floored(L2MarketMakerBaseConfig{.order_size = 1,
                                                          .inventory_limit = 100,
                                                          .tick_size = 0.5,
                                                          .signal_mode = L2SignalMode::Baseline,
                                                          .base_spread_bps = 0.0001});
        const auto floored_orders = floored.on_l2_market_view(view);
        assert(floored_orders.size() == 2);
        assert(std::abs(floored_orders[0].price - baseline_orders[0].price) < 1e-9);
        assert(std::abs(floored_orders[1].price - baseline_orders[1].price) < 1e-9);

        // 300 bps on a 6421.25 reference with a 0.5 tick is 385 ticks a side, and the two sides round outward from
        // the same fair price, so the width is 770 or 771 ticks.
        L2MarketMakerBase wide(L2MarketMakerBaseConfig{.order_size = 1,
                                                       .inventory_limit = 100,
                                                       .tick_size = 0.5,
                                                       .signal_mode = L2SignalMode::Baseline,
                                                       .base_spread_bps = 300.0});
        const auto wide_orders = wide.on_l2_market_view(view);
        assert(wide_orders.size() == 2);
        const double width_ticks = (wide_orders[1].price - wide_orders[0].price) / 0.5;
        assert(width_ticks >= 769.0 && width_ticks <= 771.0);
    }

    // The one-sided retreat: from a fraction of the cap the adding side stops opening, so the position is worked down
    // by the closing side alone instead of being carried into the mark. Two contracts against a cap of four is
    // exactly the half the fraction names, and without the flag the same position quotes both sides.
    {
        const auto drive = [&](const L2MarketMakerBaseConfig& config) {
            L2MarketMakerBase strategy_with_flag(config);
            const auto both = strategy_with_flag.on_l2_market_view(view);
            assert(both.size() == 2);
            strategy_with_flag.on_order_submitted(
                StrategyOrder{Side::Buy, view.symbol.str(), both[0].price, 2, 10});
            strategy_with_flag.on_order_update(ExecutionReport{.order_id = 10,
                                                              .side = exchange::Side::Buy,
                                                              .exec_type = ExecType::Trade,
                                                              .symbol = view.symbol.str(),
                                                              .price = both[0].price,
                                                              .quantity = 2,
                                                              .owner = "MarketMaker"});
            return strategy_with_flag.on_l2_market_view(view);
        };
        const auto control = drive(L2MarketMakerBaseConfig{.order_size = 1,
                                                           .base_spread = 1.0,
                                                           .inventory_limit = 4,
                                                           .tick_size = 0.5});
        assert(control.size() == 2);
        const auto retreat = drive(L2MarketMakerBaseConfig{.order_size = 1,
                                                           .base_spread = 1.0,
                                                           .inventory_limit = 4,
                                                           .tick_size = 0.5,
                                                           .onesided_inventory_frac = 0.5});
        assert(retreat.size() == 1);
        assert(retreat[0].side == Side::Sell);
    }

    // The adverse-selection gate: a side is quoted only when the edge that fill would capture covers the maker
    // fee plus what passive fills at that distance have actually cost. The estimate arrives from the pipeline,
    // which is the only component that sees the midpoint after a fill, and the gate waits for samples before it
    // tightens, because a gate that tightened from its first decision would refuse everything and never learn.
    {
        L2MarketMakerBase gated(L2MarketMakerBaseConfig{.order_size = 1,
                                                .base_spread = 1.0,
                                                .inventory_limit = 100,
                                                .tick_size = 0.5,
                                                .markout_cover = true,
                                                .markout_cover_min_samples = 2});
        // No samples: the gate covers the fee only, which is zero here, so both sides quote and nothing is
        // attributed to the measured cost.
        assert(gated.on_l2_market_view(view).size() == 2);
        assert(gated.markout_gate_skips() == 0);

        // One sample is not enough, so the same view still quotes.
        gated.on_markout_estimate(MarkoutEstimate{.buy_near_ticks = -100.0,
                                                  .buy_far_ticks = -100.0,
                                                  .sell_near_ticks = -100.0,
                                                  .sell_far_ticks = -100.0,
                                                  .samples = 1});
        assert(gated.on_l2_market_view(view).size() == 2);

        // Two resolved fills that cost a hundred ticks each: no quote in this book can cover that, so both sides
        // are refused and the refusal is attributed to the measured cost rather than to the fee.
        gated.on_markout_estimate(MarkoutEstimate{.buy_near_ticks = -100.0,
                                                  .buy_far_ticks = -100.0,
                                                  .sell_near_ticks = -100.0,
                                                  .sell_far_ticks = -100.0,
                                                  .samples = 2});
        assert(gated.on_l2_market_view(view).empty());
        assert(gated.markout_gate_skips() == 2);

        // A distance whose fills have been profitable needs no cover beyond the fee, which is the asymmetry the
        // gate is built on: it refuses a cost, it does not demand a premium.
        L2MarketMakerBase profitable(L2MarketMakerBaseConfig{.order_size = 1,
                                                     .base_spread = 1.0,
                                                     .inventory_limit = 100,
                                                     .tick_size = 0.5,
                                                     .markout_cover = true,
                                                     .markout_cover_min_samples = 2});
        profitable.on_markout_estimate(MarkoutEstimate{.buy_near_ticks = 5.0,
                                                       .buy_far_ticks = 5.0,
                                                       .sell_near_ticks = 5.0,
                                                       .sell_far_ticks = 5.0,
                                                       .samples = 2});
        assert(profitable.on_l2_market_view(view).size() == 2);
        assert(profitable.markout_gate_skips() == 0);
    }

    // The fee the gate has to cover is not only a rate. A US share and a listed future are billed per share and
    // per contract with a zero rate, so a gate that read the rate alone treated those markets as free and quoted
    // everything; the commission arrives as `fee_per_unit` and becomes ticks through the tick's USD value, which
    // is its size times the instrument's multiplier. This book is one tick wide, so a touch quote can capture half
    // a tick of 0.5 USD each, and the commission below is compared against exactly that.
    {
        const auto gated_with = [&](double fee_per_unit, double multiplier)
        {
            L2MarketMakerBase maker(L2MarketMakerBaseConfig{.order_size = 1,
                                                    .base_spread = 1.0,
                                                    .inventory_limit = 100,
                                                    .tick_size = 0.5,
                                                    .fee_per_unit = fee_per_unit,
                                                    .multiplier = multiplier,
                                                    .markout_cover = true,
                                                    .markout_cover_min_samples = 2});
            return maker;
        };
        // No commission: the gate covers nothing and both sides quote, which is the behaviour every schedule
        // that existed before the futures entry still gets.
        auto free_maker = gated_with(0.0, 1.0);
        assert(free_maker.on_l2_market_view(view).size() == 2);
        assert(free_maker.edge_gate_skips() == 0);

        // A commission the quote cannot cover. The quote sits a couple of ticks from the midpoint, and five
        // dollars a unit on a half dollar tick is ten ticks of fee, so both sides are refused and attributed to
        // the fee rather than to a measured cost. Before the commission reached the gate this run quoted both
        // sides, because the only rate it read was zero.
        auto dear = gated_with(5.0, 1.0);
        assert(dear.on_l2_market_view(view).empty());
        assert(dear.edge_gate_skips() == 2);
        assert(dear.markout_gate_skips() == 0);

        // The same commission on a contract whose tick is worth twenty times as much is half a tick, which the
        // same quote covers, so the multiplier is part of what decides whether a per-unit fee is dear.
        auto scaled = gated_with(5.0, 20.0);
        assert(scaled.on_l2_market_view(view).size() == 2);
        assert(scaled.edge_gate_skips() == 0);
    }

    // Inventory discipline: above the band the position is reduced instead of carried, by one order priced at
    // the touch on the reducing side, which the engine fills as a taker at that price.
    {
        L2MarketMakerBase flattening(L2MarketMakerBaseConfig{.order_size = 5,
                                                     .base_spread = 1.0,
                                                     .inventory_limit = 100,
                                                     .tick_size = 0.5,
                                                     .flatten_band_units = 4});
        const auto resting = flattening.on_l2_market_view(view);
        assert(resting.size() == 2);
        flattening.on_order_submitted(
            StrategyOrder{Side::Buy, view.symbol.str(), resting[0].price, 5, 31});
        flattening.on_order_update(ExecutionReport{.order_id = 31,
                                                   .side = exchange::Side::Buy,
                                                   .exec_type = ExecType::Trade,
                                                   .symbol = view.symbol.str(),
                                                   .price = resting[0].price,
                                                   .quantity = 5,
                                                   .owner = "MarketMaker"});
        // Five units against a band of four: one unit goes out at the touch, so it crosses instead of waiting.
        const auto flatten_order = flattening.on_l2_market_view(view);
        assert(flatten_order.size() == 1);
        assert(flatten_order[0].side == Side::Sell);
        assert(flatten_order[0].quantity == 1);
        assert(flatten_order[0].price <= view.top.bid_price);
        assert(flattening.flatten_orders() == 1);
        assert(flattening.flatten_quantity() == 1);

        // With the band off the same position is quoted around instead, which is what every run before the band
        // did, and nothing is attributed to flattening.
        L2MarketMakerBase carried(L2MarketMakerBaseConfig{.order_size = 5,
                                                  .base_spread = 1.0,
                                                  .inventory_limit = 100,
                                                  .tick_size = 0.5});
        const auto carried_open = carried.on_l2_market_view(view);
        carried.on_order_submitted(StrategyOrder{Side::Buy, view.symbol.str(), carried_open[0].price, 5, 41});
        carried.on_order_update(ExecutionReport{.order_id = 41,
                                                .side = exchange::Side::Buy,
                                                .exec_type = ExecType::Trade,
                                                .symbol = view.symbol.str(),
                                                .price = carried_open[0].price,
                                                .quantity = 5,
                                                .owner = "MarketMaker"});
        assert(carried.on_l2_market_view(view).size() == 2);
        assert(carried.flatten_orders() == 0);
    }

    // A fill that arrives after the report that ended the order is still the strategy's. A live venue answers a
    // cancel for an order it has already filled with `sCode 51400` -- a `Rejected` here -- and sends the fill that
    // made the order unfillable afterwards, so an order can leave the working set before its quantity is booked. A
    // strategy that forgot the id there booked the cancel and dropped the fill: the account kept the position and
    // the strategy did not, and the demo's close read `strategy_net_position=20000` against a flat account while
    // every other number was right. An id the process never submitted is still ignored -- a resumed session holds
    // orders it did not place, and that is the case `strategy_position_mismatches` exists for.
    {
        L2MarketMakerBase late_fill(L2MarketMakerBaseConfig{.order_size = 10,
                                                     .base_spread = 1.0,
                                                     .inventory_limit = 100,
                                                     .tick_size = 0.5});
        late_fill.on_order_submitted(StrategyOrder{Side::Sell, view.symbol.str(), 6421.0, 10, 51});
        late_fill.on_order_update(ExecutionReport{.order_id = 51,
                                                  .side = exchange::Side::Sell,
                                                  .exec_type = ExecType::Rejected,
                                                  .symbol = view.symbol.str(),
                                                  .quantity = 10,
                                                  .owner = "MarketMaker",
                                                  .reject_reason = RejectReason::Venue});
        assert(late_fill.working_order_count() == 0);
        assert(late_fill.net_position() == 0);
        late_fill.on_order_update(ExecutionReport{.order_id = 51,
                                                  .side = exchange::Side::Sell,
                                                  .exec_type = ExecType::Trade,
                                                  .symbol = view.symbol.str(),
                                                  .price = 6421.0,
                                                  .quantity = 10,
                                                  .owner = "MarketMaker"});
        assert(late_fill.net_position() == -10);

        const int64_t before_foreign = late_fill.net_position();
        late_fill.on_order_update(ExecutionReport{.order_id = 999,
                                                  .side = exchange::Side::Buy,
                                                  .exec_type = ExecType::Trade,
                                                  .symbol = view.symbol.str(),
                                                  .price = 6421.0,
                                                  .quantity = 10,
                                                  .owner = "MarketMaker"});
        assert(late_fill.net_position() == before_foreign);
    }

    // The same shape on the trend line, which held the order's entry for its entry price and its clip size as well
    // as its position.
    {
        TrendL2Strategy trend(TrendL2Config{.order_size = 10, .max_position = 100, .tick_size = 0.5});
        trend.on_order_submitted(StrategyOrder{Side::Buy, view.symbol.str(), view.top.ask_price, 10, 61});
        trend.on_order_update(ExecutionReport{.order_id = 61,
                                              .side = exchange::Side::Buy,
                                              .exec_type = ExecType::Cancelled,
                                              .symbol = view.symbol.str(),
                                              .quantity = 10,
                                              .owner = "MarketMaker",
                                              .cancel_reason = CancelReason::Venue});
        assert(trend.working_order_count() == 0);
        trend.on_order_update(ExecutionReport{.order_id = 61,
                                              .side = exchange::Side::Buy,
                                              .exec_type = ExecType::Trade,
                                              .symbol = view.symbol.str(),
                                              .price = view.top.ask_price,
                                              .quantity = 10,
                                              .owner = "MarketMaker"});
        assert(trend.net_position() == 10);
    }
}
