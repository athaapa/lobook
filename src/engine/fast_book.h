#pragma once
#include "constants.h"
#include <array>
#include <cstdint>
#include <vector>

// Q: Why is NULL_IDX defined as UINT32_MAX specifically?
// A: UINT32_MAX is a value that can never be a valid pool index — the pool is sized to
//    max_orders which will always be far less than 4 billion. It's also the value produced
//    by a uint32_t overflow, making bugs easy to spot. Using 0 would be dangerous because
//    0 is a valid index.
static constexpr uint32_t NULL_IDX = UINT32_MAX;

namespace Fast {
    struct Order {
        uint64_t id;
        uint64_t price;
        uint32_t qty;
        bool is_buy;

        // Q: Why store next/prev indices (uint32_t) instead of pointers (Order*)?
        // A: Two reasons. First, a pointer is 8 bytes on a 64-bit machine vs 4 bytes for
        //    uint32_t — smaller struct means more orders fit in a cache line. Second,
        //    if the orders vector is ever resized, all raw pointers would be invalidated
        //    (the vector may reallocate to a different address), causing dangling pointers.
        //    Indices into the vector remain valid regardless of reallocation.
        uint32_t next;
        uint32_t prev;
    };

    struct PriceLevel {
        uint32_t head; // oldest (highest priority)
        uint32_t tail; // latest (lowest priority)
        // Q: Why track both head and tail? Could you get away with just head?
        // A: You can't get away with just a head. You need both because you
        //    need to be able to quickly pop high priority orders from the front
        //    and add new orders to the back.
    };

    class FastOrderBook {
    public:
        void submit_order(uint64_t id, uint64_t price, uint32_t qty, bool is_buy) {
            match(id, price, qty, is_buy);
            if (qty > 0) {
                add_order(id, price, qty, is_buy);
            }
        }

        void cancel_order(uint64_t id) {
            if (id >= id_map.size() || id_map[id] == NULL_IDX)
                return;
            uint32_t order_idx = id_map[id];
            Order& order = orders[order_idx];

            // Remove it from the price list
            uint64_t price = order.price;
            auto& ladder = order.is_buy ? bids : asks;
            PriceLevel& price_level = ladder[price];
            if (price_level.head == order_idx) {
                price_level.head = order.next;
            }
            if (price_level.tail == order_idx) {
                price_level.tail = order.prev;
            }

            // Remove it from the list
            uint32_t prev = order.prev;
            uint32_t next = order.next;
            if (prev != NULL_IDX) {
                orders[prev].next = next;
            }
            if (next != NULL_IDX) {
                orders[next].prev = prev;
            }

            // Add it to the free list
            // Q: Why reuse the `next` field of the freed order to thread the free list,
            //    rather than keeping a separate data structure for free slots?
            // A: We are using an intrusive linked list because it allows us to reuse
            // already-allocated memory for the free list, so we don't need any extra data
            // structures.
            order.prev = NULL_IDX;
            order.next = next_free_order_idx;
            next_free_order_idx = order_idx;
            id_map[id] = NULL_IDX;
        }

        uint32_t get_quantity(uint64_t id) {
            if (id >= id_map.size() || id_map[id] == NULL_IDX)
                return 0;
            return orders[id_map[id]].qty;
        }

        // Q: Why do incoming_price and incoming_qty use pass-by-reference here?
        // A: Because they get modified inside of the match method and those modifications
        //    need to be reflected outside the scope of the function (particularly for partial
        //    fills).
        void match(
            uint64_t incoming_id, uint64_t& incoming_price, uint32_t& incoming_qty, bool is_buy) {
            auto& ladder = is_buy ? asks : bids;

            // Q: Why does the buy side start scanning from price 0 and the sell side
            //    from ladder.size()-1, rather than starting from the best available price?
            // A: You want to get the best possible deal for each order. In the buy case,
            //    this means buying from the cheapest sellers, and in the sell case,
            //    selling to the highest buyers.
            uint64_t start_price = is_buy ? 0 : (ladder.size() - 1);
            uint64_t end_price = incoming_price;
            int step = is_buy ? 1 : -1;

            for (int64_t p = start_price; incoming_qty > 0; p += step) {
                if (p < 0 || static_cast<uint64_t>(p) >= ladder.size())
                    continue;

                if (is_buy && static_cast<uint64_t>(p) > end_price)
                    break;
                if (!is_buy && static_cast<uint64_t>(p) < end_price)
                    break;

                PriceLevel& level = ladder[p];

                while (level.head != NULL_IDX && incoming_qty > 0) {
                    uint32_t resting_idx = level.head;
                    Order& resting = orders[resting_idx];

                    uint64_t trade_qty = std::min(incoming_qty, resting.qty);
                    resting.qty -= trade_qty;
                    incoming_qty -= trade_qty;

                    if (resting.qty == 0) {
                        id_map[resting.id] = NULL_IDX;
                        level.head = resting.next;
                        if (level.head == NULL_IDX) {
                            level.tail = NULL_IDX;
                        }
                        resting.next = next_free_order_idx;
                        next_free_order_idx = resting_idx;
                    } else {
                        break;
                    }
                }
            }
        }

        void add_order(uint64_t id, uint64_t price, uint32_t qty, bool is_buy) {
            // Q: What happens if next_free_order_idx is NULL_IDX — why silently return
            //    instead of growing the pool or signaling an error?
            // A: This is a fixed-capacity system by design. Growing the pool would require
            //    a heap allocation in the hot path which would be unacceptable for latency.
            //    The expectation is that the caller sizes the pool appropriately upfront.
            if (next_free_order_idx == NULL_IDX) {
                return;
            }

            uint32_t idx = next_free_order_idx;
            // Q: How does the free list "advance" to the next slot here?
            // A: Each order in the free list has a pointer to the next free Order.
            //    this line simply traverse the free pointer to the next available
            //    free slot.
            next_free_order_idx = orders[idx].next;

            orders[idx].id = id;
            orders[idx].qty = qty;
            orders[idx].price = price;
            orders[idx].next = NULL_IDX;
            orders[idx].prev = NULL_IDX;
            orders[idx].is_buy = is_buy;

            id_map[id] = idx;

            auto& ladder = is_buy ? bids : asks;
            if (price >= ladder.size())
                return;
            PriceLevel& level = ladder[price];
            if (level.head == NULL_IDX) {
                level.head = idx;
                level.tail = idx;
            } else {
                uint32_t old_tail = level.tail;
                orders[old_tail].next = idx;
                orders[idx].prev = old_tail;
                level.tail = idx;
            }
        }

        void init(size_t max_orders) {
            orders.resize(max_orders);
            // Q: Why does id_map have the same size as orders (max_orders), rather than
            //    being sized by the maximum possible order ID?
            // A: In this design, I assume IDs are assigned sequentially from 0, so IDs will never
            // exceed max_orders.
            id_map.resize(max_orders);
            // Q: Why initialize the free list by chaining order[i].next = i+1?
            // A: It initializes the free list to be the entire orders vector.
            //    As for why it's sequential, that is arbitrary. It's just simple.
            for (size_t i { 0 }; i < max_orders; i++) {
                orders[i].next = i + 1;
            }
            orders[max_orders - 1].next = NULL_IDX;
            next_free_order_idx = 0;
            for (size_t i { 0 }; i < MAX_PRICES; i++) {
                bids[i].head = NULL_IDX;
                bids[i].tail = NULL_IDX;
                asks[i].head = NULL_IDX;
                asks[i].tail = NULL_IDX;
            }
        }

    private:
        // Q: Why use a contiguous std::vector<Order> instead of allocating each Order
        //    with new (i.e., a pool/arena vs. per-object heap allocation)?
        // A: I want to guarantee that the orders are stored contiguously in memory. A vector is
        //    the easiest way to do that.
        std::vector<Order> orders;
        // Q: Why use std::array<PriceLevel, MAX_PRICES> instead of std::vector<PriceLevel>
        //    for the price ladders?
        // A: MAX_PRICES is a compile-time constant, so the size is known at compile time.
        //    std::array allocates on the stack (or statically), avoids heap allocation entirely,
        //    and has zero runtime overhead. std::vector would heap-allocate and add an
        //    indirection through a pointer.
        std::array<PriceLevel, MAX_PRICES> bids;
        std::array<PriceLevel, MAX_PRICES> asks;
        // Q: Why use a vector<uint32_t> for id_map instead of std::unordered_map<uint64_t,
        // uint32_t>? A: I don't need a unordered_map. The ids are just integers, so I can use a
        // vector.
        std::vector<uint32_t> id_map;
        uint32_t next_free_order_idx;
    };
} // namespace Fast
