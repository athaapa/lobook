#pragma once
#include "constants.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <optional>

namespace Fast {

// 3-level hierarchical bitset for tracking occupied price levels.
// Level 0 (l0): single 64-bit word — each bit summarizes one l1 word
// Level 1 (l1): array of 64-bit words — each bit summarizes one l2 word
// Level 2 (l2): array of 64-bit words — each bit represents an actual price index

class PriceBitset {
public:
    static constexpr size_t BLOCK_SIZE = 64;
    static constexpr size_t L2_BLOCKS = (MAX_PRICES + BLOCK_SIZE - 1) / BLOCK_SIZE;
    static constexpr size_t L1_BLOCKS = (L2_BLOCKS + BLOCK_SIZE - 1) / BLOCK_SIZE;

    PriceBitset()
    {
        l0 = 0;
        std::memset(l1, 0, sizeof(l1));
        std::memset(l2, 0, sizeof(l2));
    }

    ~PriceBitset() = default;

    void set(uint32_t price_idx)
    {
        assert(price_idx < MAX_PRICES);

        uint32_t l2_idx = price_idx / 64;
        uint64_t l2_bit_mask = static_cast<uint64_t>(1) << (price_idx % 64);
        l2[l2_idx] |= l2_bit_mask;

        uint32_t l1_idx = l2_idx / 64;
        uint64_t l1_bit_mask = static_cast<uint64_t>(1) << (l2_idx % 64);
        l1[l1_idx] |= l1_bit_mask;

        uint64_t l0_bit_mask = static_cast<uint64_t>(1) << l1_idx;
        l0 |= l0_bit_mask;
    }

    void clear(uint32_t price_idx)
    {
        assert(price_idx < MAX_PRICES);

        uint32_t l2_idx = price_idx / 64;
        uint64_t l2_bit_mask = static_cast<uint64_t>(1) << (price_idx % 64);
        l2[l2_idx] &= ~l2_bit_mask;

        if (l2[l2_idx] == 0) {
            uint32_t l1_idx = l2_idx / 64;
            uint64_t l1_bit_mask = static_cast<uint64_t>(1) << (l2_idx % 64);
            l1[l1_idx] &= ~l1_bit_mask;
            if (l1[l1_idx] == 0) {
                uint64_t l0_bit_mask = static_cast<uint64_t>(1) << l1_idx;
                l0 &= ~l0_bit_mask;
            }
        }
    }

    bool test(uint32_t price_idx) const
    {
        assert(price_idx < MAX_PRICES);
        uint32_t l2_idx = price_idx / 64;
        return (l2[l2_idx] >> (price_idx % 64)) & 1;
    }

    bool empty() const { return l0 == 0; }

    void clear_all()
    {
        l0 = 0;
        std::memset(l1, 0, sizeof(l1));
        std::memset(l2, 0, sizeof(l2));
    }

    std::optional<uint32_t> find_highest() const
    {
        if (l0 == 0)
            return std::nullopt;

        int l1_idx = 63 - __builtin_clzll(l0);
        int l2_idx = (l1_idx * 64) + (63 - __builtin_clzll(l1[l1_idx]));
        return static_cast<uint32_t>((64 * l2_idx) + (63 - __builtin_clzll(l2[l2_idx])));
    }

    std::optional<uint32_t> find_lowest() const
    {
        if (l0 == 0)
            return std::nullopt;

        int l1_idx = __builtin_ctzll(l0);
        int l2_idx = (l1_idx * 64) + __builtin_ctzll(l1[l1_idx]);
        return static_cast<uint32_t>((64 * l2_idx) + __builtin_ctzll(l2[l2_idx]));
    }

private:
    uint64_t l0;
    uint64_t l1[L1_BLOCKS];
    uint64_t l2[L2_BLOCKS];
};

} // namespace Fast