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
//
// Q: Why use a 3-level hierarchy instead of a flat bitset (one array of uint64_t)?
// A: Much of the value of a hierarchical bitset is the ability to quickly query the
//    highest and the lowest price levels with at least one order. If we used a flat bitset,
//    it would just be O(N) again.
// Q: Why are there exactly 3 levels rather than 2 or 4?
// A: With 2 levels the summary layer has 100k/64 = 1563 words — still O(N) to scan.
//    A third level compresses those 1563 words into ceil(1563/64) = 25 words, which
//    then fits into a single l0 word (64 bits). So find_highest/lowest always does
//    exactly 3 fixed-depth lookups — O(1) regardless of how many prices are active.

class PriceBitset {
public:
    static constexpr size_t BLOCK_SIZE = 64;
    // Q: Why is the formula (MAX_PRICES + BLOCK_SIZE - 1) / BLOCK_SIZE used for rounding up?
    // A: Integer division truncates. (7 / 4 = 1, but we need 2 blocks to hold 7 items).
    //    Adding (BLOCK_SIZE - 1) before dividing forces a round-up: (7 + 3) / 4 = 2.
    //    This is the standard ceiling division idiom: ceil(a/b) = (a + b - 1) / b.
    static constexpr size_t L2_BLOCKS = (MAX_PRICES + BLOCK_SIZE - 1) / BLOCK_SIZE;
    static constexpr size_t L1_BLOCKS = (L2_BLOCKS + BLOCK_SIZE - 1) / BLOCK_SIZE;

    PriceBitset()
    {
        l0 = 0;
        // Q: Why use std::memset to zero the arrays instead of = {} or std::fill?
        // A: std::memset is typically implemented with vectorized SIMD instructions and is highly
        //           optimized for bulk zeroing.
        std::memset(l1, 0, sizeof(l1));
        std::memset(l2, 0, sizeof(l2));
    }

    ~PriceBitset() = default;

    void set(uint32_t price_idx)
    {
        assert(price_idx < MAX_PRICES);

        uint32_t l2_idx = price_idx / 64;
        // Q: Why is the cast to uint64_t necessary before the left shift?
        // A: For safety. Integers are not guaranteed to be 64 bits, but price_idx % 64 is
        // guaranteed
        //    to be within 0 and 63. If price_idx % 64 > 31 bits it coud lead to overflow.
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

        // Q: Why do we only propagate a clear upward to l1/l0 conditionally (when the
        //    lower level word becomes zero), rather than always clearing the parent bit?
        // A: A parent bit being 1 means "at least one child bit is still set. If you always cleared
        // the parent whenever any child bit was cleared,
        //    you'd prematurely mark a group as empty even though other prices in
        //    that same word still have orders.
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
        // Q: Why read directly from l2 here instead of checking all three levels?
        // A: We are given the price_idx, so we can just directly read the value of
        //    of the bit representing the price_level in l2.
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

        // Q: What does __builtin_clzll do, and why is the result subtracted from 63?
        // A: __builtin_clzll counts the number of leading zeros in the binary representation
        //    of an integer. It is sutracted from 63 to find the index of the highest price
        //    at the next lowest level.
        int l1_idx = 63 - __builtin_clzll(l0);
        int l2_idx = (l1_idx * 64) + (63 - __builtin_clzll(l1[l1_idx]));
        return static_cast<uint32_t>((64 * l2_idx) + (63 - __builtin_clzll(l2[l2_idx])));
    }

    std::optional<uint32_t> find_lowest() const
    {
        if (l0 == 0)
            return std::nullopt;

        // Q: What does __builtin_ctzll do, and why does it give the lowest set bit
        //    directly without any subtraction?
        // A: __builtin_ctlll counts the number of trailing zeros in the binary representation
        //    of an integer. It doesn't need any subtraction because trailing zeros implicitly gives
        //    you the index. The count of trailing zeros is the bit position of the lowest set bit
        //    by definition (bit 0 = 0 trailing zeros, bit 1 = 1 trailing zero, etc.).
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
