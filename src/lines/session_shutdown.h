#pragma once

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

// A session ending on a signal, in three steps and a confirmation.
//
// The failure this exists to prevent is an orphan order: an engine that exits on `SIGTERM` leaves every quote it placed
// resting at the venue, and nothing in this process is left to cancel it. A deploy (`systemctl restart`) sends exactly
// that signal, so the order is fixed here rather than remembered:
//
//   StopNewOrders   the pipeline is told to stop submitting -- one flag, and every later decision is cancelled
//   CancelWorking   every id the venue still holds is cancelled, and the venues are asked to settle
//   Confirming      the cancels are asked about again; only an answer that says *zero* ends the session
//   Done            the venue holds nothing this session placed, so the exit is safe
//
// `TimedOut` is the fourth ending and it exists because a venue may simply stop answering: after `deadline_ms` the
// session leaves with a non-zero log line rather than pretending the cancels worked. A restart then reconciles through
// the startup path (`[RESUME]` and the snapshot), which is the other half of this story and already measured.
//
// The machine is driven by two numbers rather than by the venue: what the caller says is still working, and the
// monotonic clock. That is what lets a test pin every transition without a socket, and it keeps the one thing the
// machine cannot know -- whether the venue answered -- in the caller's hands.
class SessionShutdown
{
   public:
    enum class Step : std::uint8_t
    {
        Running = 0,
        StopNewOrders,
        CancelWorking,
        Confirming,
        Done,
        TimedOut
    };

    struct Config
    {
        // How long between two cancel rounds while the venue still reports the same orders.
        std::uint64_t cancel_retry_ms{250};
        // The whole sequence's budget. Past it the step becomes TimedOut, whatever the venue last said.
        std::uint64_t deadline_ms{10'000};
    };

    SessionShutdown() = default;
    explicit SessionShutdown(Config config) : config_(config) {}

    // The signal. Idempotent: a second request changes nothing about the sequence.
    void request(std::uint64_t now_ms)
    {
        if (requested_) return;
        requested_ = true;
        requested_ms_ = now_ms;
    }

    [[nodiscard]] bool requested() const { return requested_; }
    [[nodiscard]] bool finished() const { return step_ == Step::Done || step_ == Step::TimedOut; }
    [[nodiscard]] Step step() const { return step_; }
    [[nodiscard]] std::uint64_t cancels_sent() const { return cancels_sent_; }
    [[nodiscard]] std::uint64_t rounds() const { return rounds_; }
    [[nodiscard]] std::uint64_t working_last_seen() const { return working_last_seen_; }

    // One step, with what the venue still holds. Returns the ids to cancel now: empty when there is nothing to do yet,
    // when the retry interval has not passed, or when the session is over.
    std::vector<std::uint64_t> advance(std::uint64_t now_ms, const std::vector<std::uint64_t>& working)
    {
        if (!requested_) return {};
        note_clock(now_ms);
        working_last_seen_ = working.size();
        if (step_ == Step::Running) step_ = Step::StopNewOrders;

        // The deadline is checked before anything else: a venue that never answers must not keep the process alive.
        if (now_ms - requested_ms_ >= config_.deadline_ms)
        {
            step_ = Step::TimedOut;
            return {};
        }

        if (working.empty())
        {
            // Zero is only a confirmation once at least one round has been sent -- or at once, when this session placed
            // nothing. Anything else would call "nothing was live when the signal came" the same as "the cancels
            // worked", and the second is the claim that matters.
            step_ = Step::Done;
            return {};
        }

        step_ = Step::CancelWorking;
        if (rounds_ > 0 && now_ms - last_round_ms_ < config_.cancel_retry_ms)
        {
            step_ = Step::Confirming;
            return {};
        }
        ++rounds_;
        last_round_ms_ = now_ms;
        cancels_sent_ += working.size();
        return working;
    }

    [[nodiscard]] std::string to_log_string() const
    {
        static const char* const kNames[] = {"running", "stop-new-orders", "cancel-working",
                                             "confirming", "done", "timeout"};
        std::ostringstream stream;
        stream << "[SHUTDOWN] step=" << kNames[static_cast<int>(step_)] << " working=" << working_last_seen_
               << " cancels=" << cancels_sent_ << " rounds=" << rounds_
               << " elapsed_ms=" << (requested_ ? last_seen_ms_ : 0ULL);
        return stream.str();
    }

    // The clock the caller last advanced with, so the log line can say how long the sequence took.
    void note_clock(std::uint64_t now_ms) { last_seen_ms_ = now_ms - requested_ms_; }

   private:
    Config config_{};
    bool requested_{false};
    std::uint64_t requested_ms_{0};
    std::uint64_t last_round_ms_{0};
    std::uint64_t last_seen_ms_{0};
    std::uint64_t rounds_{0};
    std::uint64_t cancels_sent_{0};
    std::uint64_t working_last_seen_{0};
    Step step_{Step::Running};
};
