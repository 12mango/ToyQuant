#pragma once

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

#include "accounting/portfolio.h"
#include "lines/paper_broker.h"

/*
-------------------------------------------
 reconcile_positions
-------------------------------------------

What a session's two accounts say about each other, which is the check Stage B is for rather than a
summary of the run. The broker's statement is built from the reports the venue sent; the portfolio is
built from the reports the strategy received. A fill the strategy never heard about is therefore a
position that disagrees with a statement saying it happened, and that disagreement is the only thing
this function has to find.

Only this strategy's own executions are compared, because the statement already filters to them: two
different populations would break on every venue print that was somebody else's.

Cash is deliberately not compared. The broker's USD arithmetic would be a second implementation of the
portfolio's instrument model, and a second implementation that agrees only because it was copied is not
a check; quantity, fill count and fees are quantities both sides count independently.

A fee is compared with a tolerance, because splitting one fill into two adds the parts in a different
order from the total they came from and the last bit can differ without anything being wrong.
-------------------------------------------
*/

struct ReconciliationSummary
{
    uint64_t symbols_checked{0};
    // Symbols whose position the two accounts disagree about, the largest single disagreement in
    // quantity units, and the signed sum of all of them. A dropped buy and a dropped sell can leave the
    // last one flat while two symbols disagree, which is why the count and the maximum sit next to it.
    uint64_t quantity_breaks{0};
    int64_t max_quantity_break{0};
    int64_t net_quantity_break{0};
    // Quantity traded on one side and not the other, over both directions. A lost fill that a lost fill
    // the other way happens to cancel leaves the position right and still shows here, which is the only
    // column that catches it: the position is what the run is worth, and this is what it did.
    uint64_t gross_quantity_break{0};
    uint64_t report_count_difference{0};
    double fee_difference{0.0};
    bool fees_match{true};

    bool broken() const
    {
        return quantity_breaks > 0 || gross_quantity_break > 0 || report_count_difference > 0 ||
               !fees_match;
    }

    std::string to_log_string() const;
};

ReconciliationSummary reconcile_positions(const BrokerStatement& statement, const Portfolio& books);

/*
-------------------------------------------
 reconcile_equity
-------------------------------------------

What the venue says the account is worth against what the ledger says, which is the half of Stage B that
`reconcile_positions` cannot reach: the statement there is built from the venue's reports, so a fee the venue
booked but never reported, a funding payment, or a fill on another instrument leaves the two accounts equal
while the balances are not.

Both sides are optional because both can be absent for an honest reason: the venue may not answer a balance
query (a socket-only route, a refused call), and the ledger may have a position it could not mark. An absent
side is `n/a` and `status=unknown`, never a zero that reads like agreement. The tolerance is in USD and is
stated in the line, so the reader can see the width of the claim rather than only its verdict.
-------------------------------------------
*/

struct VenueEquityCheck
{
    bool venue_known{false};
    bool ledger_known{false};
    double venue_equity_usd{0.0};
    double ledger_equity_usd{0.0};
    double equity_break_usd{0.0};
    double tolerance_usd{0.05};

    // A break is a difference the tolerance does not cover. Unknown is not ok: a check that could not be made
    // must not read like one that passed.
    bool broken() const
    {
        return venue_known && ledger_known && std::abs(equity_break_usd) > tolerance_usd;
    }

    std::string to_log_string() const;
};

VenueEquityCheck reconcile_equity(const std::optional<double>& venue_equity_usd,
                                  const std::optional<double>& ledger_equity_usd,
                                  double tolerance_usd = 0.05);
