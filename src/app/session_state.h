#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "accounting/portfolio.h"
#include "common/types.h"

class Pipeline;
class IMatchingEngine;

// One order the session still had at the venue when the session ended, as far as its own records knew.
//
// It carries no quantity on purpose: the ledger keeps what an order *filled*, not what it was submitted for,
// and the one thing this record has to support is asking the venue whether the order is still alive. An
// adoption -- taking a live order over instead of cancelling it -- would need the quantity and a queue
// position nobody can reconstruct, which is why the recovery cancels instead.
struct SessionOrder
{
    uint64_t id{0};
    std::string symbol;
    Side side{Side::Unknown};
    double price{0.0};
    uint64_t filled_quantity{0};
};

// A session written down: what the account was worth, what it held, where the market was, and which orders may
// still be alive at the venue.
//
// The distinction that matters is the one between what a process can *know* and what it can only *ask*. The
// account and the marks are knowledge -- nothing else has them -- while the orders are a claim about the venue
// that only the venue can confirm, which is why `working_orders` is a list to be queried rather than a state to
// be trusted.
struct SessionSnapshot
{
    std::string symbol;
    // The market clock of the last event the session saw. A resumed run replays the file from its first line so
    // the book is the one this snapshot was taken against, and holds its own submissions until the clock passes
    // this moment.
    uint64_t last_ts{0};
    // Where the next order id has to start: reusing one the venue has already seen would be refused as a
    // duplicate, which is the one thing a resume must not do.
    uint64_t next_order_id{1};
    Portfolio::State account;
    // The last midpoint each symbol showed, because the account is marked at prices nobody has seen since.
    std::unordered_map<std::string, double> marks;
    std::vector<SessionOrder> working_orders;

    // A line-oriented text form, in the same spirit as the other files this project writes: readable, ordered,
    // and exact for a double (the streams are written with the shortest form that reads back identically).
    std::string to_text() const;
    static bool from_text(const std::string& text, SessionSnapshot& out, std::string& error);
};

// What a recovery found, which is one line of the run's output. Every order the snapshot listed is resolved into
// one of two buckets -- the venue still had it and we cancelled it, or the venue did not know it any more -- so
// the two add up to `orders` and a count that does not add up is a bug rather than a policy.
struct ResumeSummary
{
    std::string symbol;
    uint64_t last_ts{0};
    uint64_t orders{0};
    uint64_t orders_live{0};
    uint64_t orders_gone{0};
    uint64_t cancelled{0};
    double cash_usd{0.0};
    uint64_t legs{0};
    uint64_t next_order_id{1};

    std::string to_log_string() const;
};

// The snapshot of the session as it stands: the account's own state, the marks the pipeline last saw, the orders
// its ledger still holds, and the clock and id the run has reached. The symbol is passed rather than inferred,
// because a snapshot has to name the instrument it belongs to even when the session never saw a quote.
SessionSnapshot capture_session(const Pipeline& pipeline, const Portfolio& account,
                                const std::string& symbol);

// Take a session over: put the account back where the snapshot says it was, then resolve every order the
// snapshot listed against the venue. An order the venue still holds is cancelled -- a queue position a new
// process cannot account for is not a position it can keep -- and one the venue has forgotten is counted rather
// than assumed to be either filled or cancelled, because the venue's query cannot tell those apart.
//
// The strategy is deliberately not restored, and nothing here pretends otherwise: a new process starts with the
// account it was given and no memory of its own quotes. What that costs is measured by
// `strategy_position_mismatches` on the first fill, which is the counter that exists for exactly this.
ResumeSummary resume_session(const SessionSnapshot& snapshot, Portfolio& account, IMatchingEngine& venue);

// The two halves a caller actually needs. `write` refuses to leave a half-written file behind; `read` refuses a
// snapshot it cannot parse rather than resuming from a partial one.
bool write_snapshot(const std::string& path, const SessionSnapshot& snapshot, std::string& error);
bool read_snapshot(const std::string& path, SessionSnapshot& out, std::string& error);
