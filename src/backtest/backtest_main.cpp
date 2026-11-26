#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include "backtest/backtest_driver.h"
#include "utils/logger.h"

#ifndef PROJECT_ROOT_DIR
#define PROJECT_ROOT_DIR "."
#endif

// Relative paths are the operator's: they resolve against where the process runs, not against the tree it was built in. The
// shipped binary carried that tree once -- the CI runner's own checkout directory -- and died on the host when it tried to
// create its log directory there. `TOYQUANT_ROOT` pins the root when a run has to start from somewhere else.
std::string to_abs_path(const std::string& input_path)
{
    namespace fs = std::filesystem;
    fs::path p(input_path);
    if (p.is_absolute())
    {
        return p.string();
    }
    if (const char* root = std::getenv("TOYQUANT_ROOT"); root != nullptr && *root != '\0')
    {
        return (fs::path(root) / p).string();
    }
    std::error_code error;
    const fs::path here = fs::current_path(error);
    if (error)
    {
        return p.string();
    }
    return (here / p).string();
}

int main(int argc, char** argv)
{
    // Default relative paths.
    std::string tick_file = "data/scenarios/synthetic_ticks.csv";
    std::string orders_file = "data/runtime/orders.csv";
    std::string trades_file = "data/runtime/trades.csv";
    std::string log_file = "logs/backtest.log";

    double slippage = 0.0;
    double fee_rate = 0.0;
    RunMode mode = RunMode::Backtest;

    // 1. Read command-line arguments.
    if (argc > 1) tick_file = argv[1];
    if (argc > 2) orders_file = argv[2];
    if (argc > 3) trades_file = argv[3];
    if (argc > 4) slippage = std::atof(argv[4]);
    if (argc > 5) fee_rate = std::atof(argv[5]);
    if (argc > 6)
    {
        // Accepted and reported for CLI compatibility. BacktestDriver evaluates both values
        // identically, so nothing downstream branches on it.
        std::string m = argv[6];
        mode = (m == "realtime" ? RunMode::Realtime : RunMode::Backtest);
    }
    if (argc > 7) log_file = argv[7];

    // 2. Convert paths to absolute paths rooted at the project directory.
    tick_file = to_abs_path(tick_file);
    orders_file = to_abs_path(orders_file);
    trades_file = to_abs_path(trades_file);
    log_file = to_abs_path(log_file);

    try
    {
        Logger logger(log_file);
        if (!std::filesystem::is_regular_file(tick_file))
        {
            logger.error("Error: tick file does not exist: ", tick_file);
            return 1;
        }
        if (!std::filesystem::is_regular_file(trades_file))
        {
            logger.error("Error: trades file does not exist: ", trades_file);
            return 1;
        }

        logger.log("[Runtime CWD]: ", std::filesystem::current_path(), "\n",
                   "[Project Root]:", PROJECT_ROOT_DIR, "\n", "Tick file:     ", tick_file, "\n",
                   "Orders file:   ", orders_file, "\n", "Trades file:   ", trades_file, "\n",
                   "Slippage:      ", slippage, "\n", "Fee rate:      ", fee_rate, "\n",
                   "Mode:          ", (mode == RunMode::Realtime ? "Realtime" : "Backtest"), "\n",
                   "Log file:      ", log_file);

        legacy::BacktestDriver driver(tick_file, orders_file, trades_file, slippage, fee_rate,
                                      logger);
        driver.run();
    }
    catch (const std::exception& ex)
    {
        std::cerr << "Error: " << ex.what() << std::endl;
        return 1;
    }

    return 0;
}