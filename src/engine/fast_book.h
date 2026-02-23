#pragma once
#include <array>
#include <cstdint>
#include <vector>
#include "constants.h"

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
    uint32_t head; // oldest;
    uint32_t tail; // latest;
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
        Order &order = orders[order_idx];

        // Remove it from the price list
        uint64_t price = order.price;
        auto& ladder = order.is_buy ? bids : asks;
        PriceLevel &price_level = ladder[price];
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

    void match(uint64_t incoming_id, uint64_t &incoming_price,
               uint32_t &incoming_qty, bool is_buy) {
        auto &ladder = is_buy ? asks : bids;

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

            PriceLevel &level = ladder[p];

            while (level.head != NULL_IDX && incoming_qty > 0) {
                uint32_t resting_idx = level.head;
                Order &resting = orders[resting_idx];

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

        auto &ladder = is_buy ? bids : asks;
        if (price >= ladder.size())
            return;
        PriceLevel &level = ladder[price];
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
        id_map.resize(max_orders);
        for (size_t i{0}; i < max_orders; i++) {
            orders[i].next = i + 1;
        }
        orders[max_orders - 1].next = NULL_IDX;
        next_free_order_idx = 0;
        for (size_t i{0}; i < MAX_PRICES; i++) {
            bids[i].head = NULL_IDX;
            bids[i].tail = NULL_IDX;
            asks[i].head = NULL_IDX;
            asks[i].tail = NULL_IDX;
        }
    }

  private:
    std::vector<Order> orders; // Ensures that the orders are next to each other
    std::array<PriceLevel, MAX_PRICES>
        bids; // Price ladder for bids. Allows O(1) access (std::map has pointer
              // chasing issue)
    std::array<PriceLevel, MAX_PRICES> asks; // See above.
    std::vector<uint32_t>
        id_map; // O(1) access of order by id. Don't need a map for this because
                // the ids are just numbers.
    uint32_t next_free_order_idx; // Stores the free list (list of "spaces" for
                                  // an order which are available)
};
} // namespace Fast
