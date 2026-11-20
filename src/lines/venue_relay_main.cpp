// The CLI of the C++ relay. It mirrors `tools/live/venue_relay.py`'s flags so the two are interchangeable:
// the engine connects to `--listen`, the venue is `--venue`, and a window can be recorded directly or
// captured raw and replayed with no network.

#include <cstdlib>
#include <exception>
#include <iostream>
#include <map>
#include <string>

#include "lines/venue_relay.h"
#include "lines/venue_ws.h"

namespace
{
void usage(const char* executable)
{
    std::cerr << "Usage: " << executable << " [options]\n"
              << "  --venue=URL             the venue's websocket url (default OKX public)\n"
              << "  --venue-demo            the demo venue's public stream (wss://wspap.okx.com:8443/ws/v5/public)\n"
              << "  --instrument=SYM        the instrument to subscribe (default BTC-USDT-SWAP)\n"
              << "  --books-channel=CH      books | books5 | bbo-tbt (default books)\n"
              << "  --levels=N              levels kept from a snapshot, 0 keeps all (default 25)\n"
              << "  --contract-size=X       contract value for the quantity scale (default 0.01)\n"
              << "  --quantity-scale=N      engine units per base unit (default 1000000)\n"
              << "  --listen=HOST:PORT      serve the engine here; empty disables serving\n"
              << "  --order-listen=HOST:PORT serve the engine's order leg here; empty disables it\n"
              << "  --order-venue=URL       the venue's private websocket url for that leg (default: its demo endpoint)\n"
              << "  --order-rest=URL        place over REST at this base url instead of the socket's trade operation\n"
              << "  --order-inst-code=N     the venue's own code for the contract, sent when known; -1 leaves it out\n"
              << "  --proxy=HOST:PORT       reach the venue through this HTTP CONNECT proxy; absent dials directly\n"

              << "  --order-drop-after-seconds=X  close and reopen the private connection every X seconds (reconnect test)\n"
              << "  --order-dump=FILE       append every frame both ways, one JSON object a line (credentials masked)\n"
              << "  --tick-size=X           the price step the venue accepts on this instrument (default 0.1)\n"
              << "  --lot-size=N            the size step, in contracts, the venue accepts (default 1)\n"
              << "  --capture=FILE          append the venue's raw messages, for --replay\n"
              << "  --record=PREFIX         write PREFIX_trades.csv and PREFIX_depth.csv directly\n"
              << "  --replay=FILE           serve a capture instead of connecting; no network\n"
              << "  --max-seconds=X         stop after X seconds; 0 runs until stopped\n"
              << "  --ping-seconds=X        ping every X seconds even while data flows; 0 only when idle\n"
              << "  --drop-after-seconds=X  close and reopen the venue connection every X seconds (reconnect test)\n"
              << "  --self-test             decode the shapes a live capture showed, then exit\n";
}

std::string option(const std::map<std::string, std::string>& options, const std::string& name,
                   const std::string& fallback)
{
    const auto found = options.find(name);
    return found == options.end() ? fallback : found->second;
}
}  // namespace

int main(int argc, char** argv)
{
    // Before anything opens a socket: a venue that resets a TLS connection mid-write must cost the write, not the
    // process (see `venue_ws::ignore_sigpipe`).
    venue_ws::ignore_sigpipe();
    RelayConfig config;
    std::map<std::string, std::string> options;
    bool self_test = false;
    bool venue_demo = false;
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        if (argument == "--self-test")
        {
            self_test = true;
            continue;
        }
        // A shorthand rather than a value: the demo venue's public stream, which is the one the demo account's own
        // instrument ids belong to. `--venue` and this name the same endpoint, so giving both is refused below rather
        // than resolved by argument order.
        if (argument == "--venue-demo")
        {
            venue_demo = true;
            continue;
        }
        if (argument == "--help" || argument == "-h")
        {
            usage(argv[0]);
            return 0;
        }
        if (argument.rfind("--", 0) != 0)
        {
            std::cerr << "Error: unexpected argument " << argument << "\n";
            return 1;
        }
        const std::size_t equals = argument.find('=');
        if (equals != std::string::npos)
        {
            options[argument.substr(0, equals)] = argument.substr(equals + 1);
        }
        else
        {
            if (index + 1 >= argc)
            {
                std::cerr << "Error: " << argument << " needs a value\n";
                return 1;
            }
            options[argument] = argv[++index];
        }
    }

    try
    {
        if (venue_demo && options.count("--venue") != 0)
        {
            std::cerr << "Error: --venue and --venue-demo name the same endpoint; give one\n";
            return 1;
        }
        config.venue = venue_demo ? std::string("wss://wspap.okx.com:8443/ws/v5/public")
                                  : option(options, "--venue", config.venue);
        config.instrument = option(options, "--instrument", config.instrument);
        config.books_channel = option(options, "--books-channel", config.books_channel);
        config.listen = option(options, "--listen", config.listen);
        config.capture = option(options, "--capture", config.capture);
        config.record = option(options, "--record", config.record);
        config.replay = option(options, "--replay", config.replay);
        const auto present = [&options](const std::string& name) { return options.count(name) != 0; };
        if (present("--levels")) config.levels = std::stoul(options["--levels"]);
        if (present("--contract-size")) config.contract_size = std::stod(options["--contract-size"]);
        if (present("--quantity-scale"))
            config.quantity_scale = std::stoull(options["--quantity-scale"]);
        if (present("--max-seconds")) config.max_seconds = std::stod(options["--max-seconds"]);
        if (present("--ping-seconds")) config.ping_seconds = std::stod(options["--ping-seconds"]);
        if (present("--drop-after-seconds"))
            config.drop_after_seconds = std::stod(options["--drop-after-seconds"]);

        config.order_listen = option(options, "--order-listen", config.order_listen);
        config.order_venue = option(options, "--order-venue", config.order_venue);
        config.order_rest = option(options, "--order-rest", config.order_rest);
        // A tunnel when the venue is only reachable through one: `--proxy host:port`, direct when the flag is absent. The
        // value is parsed rather than stored, so a typo is an error at start-up instead of a direct connection nobody
        // asked for.
        config.proxy = venue_ws::parse_proxy(option(options, "--proxy", ""));
        config.order_dump = option(options, "--order-dump", config.order_dump);
        if (present("--order-inst-code")) config.order_inst_code = std::stoll(options["--order-inst-code"]);
        if (present("--tick-size")) config.tick_size = std::stod(options["--tick-size"]);
        if (present("--lot-size")) config.lot_size = std::stoll(options["--lot-size"]);
        if (present("--order-drop-after-seconds"))
            config.order_drop_after_seconds = std::stod(options["--order-drop-after-seconds"]);
        if (config.order_listen.empty())
        {
            // Named rather than accepted and ignored: each of these describes the order leg, and a run without that
            // listener would place nothing while looking configured.
            for (const char* name : {"--order-venue", "--order-rest", "--order-inst-code", "--tick-size", "--lot-size",
                                     "--order-drop-after-seconds", "--order-dump"})
                if (present(name))
                {
                    std::cerr << "Error: " << name << " describes the order leg, which --order-listen serves; "
                              << "without it there is no order channel to configure\n";
                    return 1;
                }
        }
        else
        {
            // The market leg's flags are refused from the other side for the same reason: this route serves the order
            // channel and returns, so a run that also named a market listener would look like both legs and be one.
            for (const char* name : {"--listen", "--capture", "--record", "--replay", "--venue"})
                if (present(name))
                {
                    std::cerr << "Error: " << name << " is the market leg's; --order-listen serves the order channel "
                              << "and returns, so the market leg is a second process\n";
                    return 1;
                }
            if (venue_demo)
            {
                std::cerr << "Error: --venue-demo is the market leg's; --order-listen serves the order channel "
                          << "and returns, so the market leg is a second process\n";
                return 1;
            }
        }

        return self_test ? relay_self_test() : run_relay(config);
    }
    catch (const std::exception& error)
    {
        std::cerr << "Error: " << error.what() << "\n";
        return 1;
    }
}
