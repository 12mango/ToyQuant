#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lines/venue_json.h"
#include "lines/venue_relay.h"

// The market leg's decoder: the venue's public-channel messages as the replay rows this repository already reads. It is a
// file of its own because `venue_relay.cpp` is a client, a queue, a recorder and two entry points around it, and because
// one decoder serves three roles -- the relay, a direct recording, and a capture replay -- so a row means the same thing
// wherever it is produced. Nothing here knows a socket exists.
struct RelayStats
{
    uint64_t trades{0};
    uint64_t book_messages{0};
    uint64_t depth_rows{0};
    uint64_t gaps{0};
    uint64_t reconnects{0};
    uint64_t parse_errors{0};
    // Backpressure and heartbeat, both of which a six second run never exercises: the high-water mark
    // is how deep the hand-off to the engine got, the waits are how often it was full, and the two
    // heartbeat counters say the keepalive path ran at all.
    uint64_t queue_high_water{0};
    uint64_t queue_waits{0};
    uint64_t pings_sent{0};
    uint64_t pongs_sent{0};
    // A pong the venue sent back, which is what makes a keepalive a round trip rather than a write.
    uint64_t pongs_received{0};

    std::string to_log_string() const;
};

class VenueDecoder
{
   public:
    explicit VenueDecoder(const RelayConfig& config);

    void decode(std::string_view message, std::vector<std::string>& rows, RelayStats& stats);

   private:
    uint64_t amount_of(const Json* size) const;
    void decode_trades(const Json& root, const std::string& symbol, std::vector<std::string>& rows,
                       RelayStats& stats);
    void decode_books(const Json& root, const std::string& symbol, std::vector<std::string>& rows,
                      RelayStats& stats);

    std::string instrument_;
    double contract_size_;
    uint64_t quantity_scale_;
    std::size_t max_levels_;
    int64_t last_seq_{0};
    bool has_last_seq_{false};
};
