#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include "lines/session_shutdown.h"

namespace
{
// The four endings a session can have, written as the sequence a caller would step through: the machine is driven by
// the clock and by what the venue still holds, so a test needs no socket and no venue to pin every transition.
int main_impl()
{
    using Step = SessionShutdown::Step;

    // A session nobody signalled does nothing, however many times it is advanced.
    {
        SessionShutdown idle;
        assert(idle.advance(1'000, {}) == std::vector<std::uint64_t>{});
        assert(idle.step() == Step::Running);
        assert(!idle.requested());
        assert(!idle.finished());
    }

    // Signalled with nothing live: the exit is safe at once, and no cancel is invented.
    {
        SessionShutdown empty;
        empty.request(1'000);
        assert(empty.advance(1'010, {}).empty());
        assert(empty.step() == Step::Done);
        assert(empty.finished());
        assert(empty.cancels_sent() == 0);
        assert(empty.to_log_string().find("step=done") != std::string::npos);
    }

    // Signalled with two orders live: both are cancelled in the first round, and the same two coming back before the
    // retry interval do not produce a second round -- a cancel per view would be a cancel storm against the venue.
    {
        SessionShutdown shutdown;
        shutdown.request(1'000);
        const std::vector<std::uint64_t> working{7, 9};
        const auto first = shutdown.advance(1'010, working);
        assert(first.size() == 2);
        assert(first[0] == 7 && first[1] == 9);
        assert(shutdown.step() == Step::CancelWorking);
        assert(shutdown.rounds() == 1);
        assert(shutdown.cancels_sent() == 2);

        // Inside the retry interval: nothing goes out, and the step says the session is confirming rather than working.
        assert(shutdown.advance(1'100, working).empty());
        assert(shutdown.step() == Step::Confirming);
        assert(shutdown.rounds() == 1);

        // Past it: the same ids are cancelled once more, which is what a venue that lost the first cancel needs.
        const auto second = shutdown.advance(1'300, working);
        assert(second.size() == 2);
        assert(shutdown.rounds() == 2);
        assert(shutdown.cancels_sent() == 4);

        // The venue now says zero: only this answer ends the session.
        assert(shutdown.advance(1'320, {}).empty());
        assert(shutdown.step() == Step::Done);
        assert(shutdown.finished());
        assert(shutdown.to_log_string().find("working=0") != std::string::npos);
    }

    // A venue that stops answering the cancels: the deadline ends the session rather than letting it hang.
    {
        SessionShutdown stuck(SessionShutdown::Config{.cancel_retry_ms = 100, .deadline_ms = 500});
        stuck.request(1'000);
        const std::vector<std::uint64_t> working{11};
        (void)stuck.advance(1'010, working);
        (void)stuck.advance(1'200, working);
        (void)stuck.advance(1'400, working);
        assert(!stuck.finished());
        (void)stuck.advance(1'500, working);
        assert(stuck.step() == Step::TimedOut);
        assert(stuck.finished());
        assert(stuck.to_log_string().find("step=timeout") != std::string::npos);
    }

    // The restart case, mocked: the set a *previous* process left is cancelled before the new session can call itself
    // stopped. That is the state a snapshot gives the startup path (`[RESUME]`), and the number this pins is that every
    // inherited id goes out on the first round -- not "the session is flat", which the venue has to say.
    {
        SessionShutdown inherited;
        inherited.request(5'000);
        const std::vector<std::uint64_t> snapshot{101, 102, 103};
        const auto cancel = inherited.advance(5'001, snapshot);
        assert(cancel.size() == snapshot.size());
        for (std::size_t index = 0; index < snapshot.size(); ++index) assert(cancel[index] == snapshot[index]);
        assert(inherited.step() == Step::CancelWorking);
        // A partial answer is not a confirmation: two of the three still live keeps the session open.
        assert(inherited.advance(5'010, {101, 102}).empty());
        assert(!inherited.finished());
        assert(inherited.advance(5'400, {}).empty());
        assert(inherited.finished());
        assert(inherited.rounds() == 1);
    }
    return 0;
}
}  // namespace

int main()
{
    return main_impl();
}
