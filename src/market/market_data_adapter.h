#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>

#include "common/instrument_spec.h"
#include "common/market_event.h"

class IMarketEventReader
{
   public:
    // What one read did. `Timeout` exists because a live socket has no end: a caller waiting for one has to be told
    // "nothing yet" without being told the stream is over -- otherwise a quiet market would end the session, and a
    // signal received while the market is quiet would never be acted on (measured 2026-11-27: a graceful stop's three
    // steps ran inside the consumer, the consumer was never called, and systemd SIGKILLed the session with its orders
    // still resting at the venue).
    enum class Status : std::uint8_t
    {
        Event,
        Timeout,
        End
    };

    virtual ~IMarketEventReader() = default;
    // `[[nodiscard]]` because the return value is the only thing that says whether `event` was written:
    // a reader with nothing left returns false and leaves the event untouched, and a caller that ignored
    // that would process the previous event a second time.
    [[nodiscard]] virtual bool next(MarketEvent& event) = 0;
    // `next` with the right to give up: no event within `wait_us` returns `Timeout` while the stream is still open.
    // A file reader keeps this default -- its `next` never blocks -- so a replay's delivery is unchanged, and a caller
    // that passes no wait (`wait_us == 0`) gets the blocking read it asked for.
    [[nodiscard]] virtual Status next_within(MarketEvent& event, std::int64_t wait_us)
    {
        (void)wait_us;
        return next(event) ? Status::Event : Status::End;
    }
};

struct MarketDataReaders
{
    std::unique_ptr<IMarketEventReader> trades;
    std::unique_ptr<IMarketEventReader> quotes;
};

MarketDataReaders make_market_data_readers(const std::string& format,
                                           const std::string& trades_path,
                                           const std::string& quotes_path,
                                           const InstrumentSpec& instrument);

std::unique_ptr<IMarketEventReader> make_deribit_book_snapshot_reader(
    const std::string& path);

std::unique_ptr<IMarketEventReader> make_deribit_incremental_book_reader(
    const std::string& path);

std::unique_ptr<IMarketEventReader> make_deribit_depth_reader(const std::string& path);

std::unique_ptr<IMarketEventReader> make_deribit_trade_reader(const std::string& path);

// Binance: the aggregate-trade CSV and the book-ticker CSV of the public data dumps, both scaled into the
// instrument's quantity unit. Declared beside the OKX pair for the same reason: the router names them, and
// so can a test that wants one reader rather than the pair.
std::unique_ptr<IMarketEventReader> make_binance_trade_reader(const std::string& path,
                                                              const std::string& symbol,
                                                              uint64_t quantity_scale);

std::unique_ptr<IMarketEventReader> make_binance_book_reader(const std::string& path,
                                                             const std::string& symbol,
                                                             uint64_t quantity_scale);

// OKX order book JSON Lines and the traded CSV that tools/fetch/fetch_okx_trades.py produces from the archive.
std::unique_ptr<IMarketEventReader> make_okx_book_reader(const std::string& path, const std::string& symbol,
                                                         double contract_size, uint64_t quantity_scale);

std::unique_ptr<IMarketEventReader> make_okx_trade_reader(const std::string& path, const std::string& symbol,
                                                          double contract_size, uint64_t quantity_scale);

// Which reader a depth file needs, decided from the file itself: an OKX archive opens with a JSON object per
// line, and the Deribit depth files are CSV with a header.
std::string market_data_format_of(const std::string& quotes_path);
