#include <exception>
#include <iostream>
#include <string>

#include "app/app_config.h"
#include "app/application.h"

#include <iostream>
#include <stdexcept>
#include <string>

#include "common/termination.h"
int main(int argc, char** argv)
{
    AppConfig cfg;
    std::string error;
    if (!parse_config(argc, argv, cfg, error))
    {
        if (!error.empty())
        {
            std::cerr << "Error: " << error << "\n";
        }
        print_usage(argv[0]);
        return error.empty() ? 0 : 1;
    }

    try
    {
        // `systemctl restart` sends SIGTERM; the handler only records it, and the session's own loop turns the record
        // into "stop new orders, cancel what is live, confirm zero, exit". Installed here so every mode has it: a replay
        // that is signalled ends cleanly with its summary, and a live session cancels before it leaves.
        termination::install();
        return Application(cfg).run();
    }
    catch (const std::exception& ex)
    {
        std::cerr << "Error: " << ex.what() << "\n";
        return 1;
    }
}