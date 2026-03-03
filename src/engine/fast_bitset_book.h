#pragma once
#include "constants.h"
#include "price_bitset.h"
#include <array>
#include <cstdint>
#include <vector>

// Q: Both fast_book.h and fast_bitset_book.h define NULL_IDX — why might this cause
//    a linker error if both headers are included in the same translation unit?
// A: It actually won't here, because static constexpr has internal linkage — each
//    translation unit gets its own private copy, so there's no ODR violation.
//    The real issue is duplication: if the value ever changed in one file but not
//    the other, you'd have a silent bug. It should be defined once in a shared header.
static constexpr uint32_t NULL_IDX = UINT32_MAX;

namespace Fast {
struct Order {
    uint64_t id;
    uint64_t price;
    uint32_t qty;
    bool is_buy;

    uint32_t next;
    uint32_t prev;
};

struct PriceLevel {
    uint32_t head; // oldest (highest priority)
    uint32_t tail; // latest (lowest priority)
};

class FastBitsetOrderBook {
public:
    void submit_order(uint64_t id, uint64_t price, uint32_t qty, bool is_buy)
    {
        match(id, price, qty, is_buy);
        if (qty > 0) {
            add_order(id, price, qty, is_buy);
        }
    }

    void cancel_order(uint64_t id)
    {
        if (id >= id_map.size() || id_map[id] == NULL_IDX)
            return;
        uint32_t order_idx = id_map[id];
        Order& order = orders[order_idx];

        // Remove it from the price list
        uint64_t price = order.price;
        auto& levels = order.is_buy ? bid_levels : ask_levels;
        PriceLevel& price_level = levels[price];
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

        // Q: Why clear the bitset bit *after* unlinking from the doubly-linked list,
        //    and only if price_level.head is now NULL_IDX?
        // A: We only want to clear the bitset bit if there are no more active orders at this price level.
        //    We do this after unlinking because then we can simply check price_level.head to see if there are any
        //    orders at this price_level.
        if (price_level.head == NULL_IDX) {
            auto& index = order.is_buy ? bid_index : ask_index;
            index.clear(price);
        }

        // Add it to the free list
        order.prev = NULL_IDX;
        order.next = next_free_order_idx;
        next_free_order_idx = order_idx;
        id_map[id] = NULL_IDX;
    }

    uint32_t get_quantity(uint64_t id)
    {
        if (id >= id_map.size() || id_map[id] == NULL_IDX)
            return 0;
        return orders[id_map[id]].qty;
    }

    void match(uint64_t incoming_id, uint64_t& incoming_price,
        uint32_t& incoming_qty, bool is_buy)
    {
        auto& levels = is_buy ? ask_levels : bid_levels;
        auto& index = is_buy ? ask_index : bid_index;

        while (incoming_qty > 0) {
            // Q: How is find_lowest()/find_highest() faster than the price-scanning
            //    loop in FastOrderBook::match()? What is the asymptotic difference?
            // A: find_lowest() and find_highest() operate in O(1) time whereas FastOrderBook::match() is in O(MAX_PRICES) time.
            auto best = is_buy ? index.find_lowest() : index.find_highest();
            if (!best)
                break;
            // Q: Why compare *best against incoming_price here — what does this check enforce?
            // A: We want to ensure that there exists outstanding orders that could fulfill the incoming orders.
            if ((is_buy && *best > incoming_price) || (!is_buy && *best < incoming_price)) {
                break;
            }
            PriceLevel& level = levels[*best];
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

            if (level.head == NULL_IDX) {
                index.clear(*best);
            }
        }
    }

    void add_order(uint64_t id, uint64_t price, uint32_t qty, bool is_buy)
    {
        if (next_free_order_idx == NULL_IDX) {
            return;
        }

        uint32_t idx = next_free_order_idx;
        next_free_order_idx = orders[idx].next;

        orders[idx].id = id;
        orders[idx].qty = qty;
        orders[idx].price = price;
        orders[idx].next = NULL_IDX;
        orders[idx].prev = NULL_IDX;
        orders[idx].is_buy = is_buy;

        id_map[id] = idx;

        auto& levels = is_buy ? bid_levels : ask_levels;
        auto& index = is_buy ? bid_index : ask_index;

        if (price >= levels.size())
            return;
        PriceLevel& level = levels[price];
        if (level.head == NULL_IDX) {
            level.head = idx;
            level.tail = idx;
            // Q: Why call index.set(price) only when the level goes from empty to non-empty,
            //    rather than on every add_order call?
            // A: A set bit only signifies that there are active orders at this price level.
            //    Therefore, we don't need to set the bit for every order, only on the first order.
            index.set(price);
        } else {
            uint32_t old_tail = level.tail;
            orders[old_tail].next = idx;
            orders[idx].prev = old_tail;
            level.tail = idx;
        }
    }

    void init(size_t max_orders)
    {
        orders.resize(max_orders);
        id_map.resize(max_orders);

        bid_index.clear_all();
        ask_index.clear_all();

        for (size_t i { 0 }; i < max_orders; i++) {
            orders[i].next = i + 1;
        }
        orders[max_orders - 1].next = NULL_IDX;
        next_free_order_idx = 0;
        for (size_t i { 0 }; i < MAX_PRICES; i++) {
            bid_levels[i].head = NULL_IDX;
            bid_levels[i].tail = NULL_IDX;
            ask_levels[i].head = NULL_IDX;
            ask_levels[i].tail = NULL_IDX;
        }
    }

private:
    std::vector<Order> orders;

    // Q: Why have separate bid_index/ask_index bitsets instead of one shared one?
    // A: When matching orders, bids and asks will have to iterate through entirely
    //    different order lists. It isn't useful for an incoming buy order to know that there are
    //    other buys at the same price level.
    PriceBitset bid_index;
    PriceBitset ask_index;

    std::array<PriceLevel, MAX_PRICES> bid_levels;
    std::array<PriceLevel, MAX_PRICES> ask_levels;

    std::vector<uint32_t> id_map;
    uint32_t next_free_order_idx;
};
} // namespace Fast
