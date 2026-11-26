#pragma once

#include <atomic>
#include <csignal>
#include <cstdlib>

// A session that is told to stop, and the one thing it must not do: stop abruptly.
//
// The process is a live venue's client, so an exit leaves whatever it placed resting at the venue. `SIGTERM` is what
// a deploy sends (`systemctl restart`), which is why this file exists rather than a bare `signal(SIGTERM, ...)` in
// `main`: the handler only records the request, and the session's own loop turns the record into the three steps
// (`SessionShutdown`: stop new orders, cancel what is live, confirm zero, exit).
//
// The handler is async-signal-safe: one lock-free store, nothing else. A second signal exits immediately, because a
// session whose cancel loop is wedged (a venue that stopped answering) still has to be restartable -- systemd escalates
// to SIGKILL after its own timeout, and an operator asking twice should not have to wait for it.
namespace termination
{

inline std::atomic<bool>& requested_flag()
{
    static std::atomic<bool> requested{false};
    return requested;
}

inline void handle(int)
{
    // First request: let the session finish its cancels. Second: give up now, with the status a killed session has.
    if (requested_flag().exchange(true)) std::_Exit(143);
}

inline void install()
{
    std::signal(SIGTERM, handle);
    std::signal(SIGINT, handle);
}

inline bool requested()
{
    return requested_flag().load();
}

}  // namespace termination
