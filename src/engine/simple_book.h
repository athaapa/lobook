#pragma once
#include <cstdint>
#include <iostream>
#include <list>
#include <map>
#include <unordered_map>

namespace Naive {
struct Order {
    uint64_t id;
    uint64_t price;
    uint32_t qty;
    bool is_buy;
};

// Q: Why store an iterator in OrderLocation instead of just the price and side?
struct OrderLocation {
    uint64_t price;
    bool is_buy;
    std::list<Order>::iterator itr;
};

class NaiveOrderBook {
public:
    int total_trades {};

    void add_order(uint64_t id, uint64_t price, uint32_t qty, bool is_buy)
    {
        Order ord = { id, price, qty, is_buy };

        // Q: Why call match() before inserting the order into the book?
        // A: The order only needs to rest (i.e. enter the book) if it can't immediately find a pair
        match(ord);

        if (ord.qty > 0) {
            // Q: Why use std::list<Order> per price level instead of std::deque or std::vector?
            // A: cancel_order needs to erase an arbitrary element by iterator. std::list gives
            //    O(1) erase at any position given an iterator. std::vector or std::deque would
            //    require O(n) shifting/scanning to remove an element from the middle.
            auto& list = is_buy ? bids[price] : asks[price];
            auto it = list.insert(list.end(), ord);
            order_lookup[id] = { price, is_buy, it };
        }
    }

    void cancel_order(uint64_t id)
    {
        auto it_lookup = order_lookup.find(id);
        if (it_lookup != order_lookup.end()) {
            OrderLocation& loc = it_lookup->second;
            if (loc.is_buy) {
                bids[loc.price].erase(loc.itr);
            } else {
                asks[loc.price].erase(loc.itr);
            }
            // Q: Why call order_lookup.erase(it_lookup) instead of order_lookup.erase(id)?
            // A: unordered_map.erase(iterator*) is O(1) time complexity. Conversely, erase(key) is average
            //    O(1), but has a worst case of O(n), particularly if there are a lot of hash collisions.
            order_lookup.erase(it_lookup);
        }
    }

    bool has_order(uint64_t id) { return order_lookup.count(id); }

    void debug_print()
    {
        std::cout << "--- BOOK STATE ---\n";
        for (auto const& [price, list] : bids) {
            std::cout << "Bid Price " << price << " has " << list.size()
                      << " orders.\n";
        }
    }

private:
    // Q: Why use std::map instead of std::unordered_map for bids and asks?
    // A: std::map sorts its keys while unordered_map does not. In this case, the sorting
    //    is convenient, because it allows us to more easily iterate through highest prices
    //    to lowest prices (or vice versa).
    // Q: Why does bids use std::greater<uint64_t> as a comparator but asks does not?
    // A: We want bids to be sorted by the highest buy price while asks are implicitly
    //    sorted by their lowest sell price. This is helpful for matching (we want to get the best deal)
    std::map<uint64_t, std::list<Order>, std::greater<uint64_t>> bids;
    std::map<uint64_t, std::list<Order>> asks;

    // Q: Why is order_lookup an unordered_map instead of a map?
    // A: order_lookup is only ever accessed by exact ID — we never iterate it in sorted
    //    order or need the best/worst price. unordered_map gives O(1) lookup, insert, and
    //    erase without the overhead of tree rebalancing that a map would pay.
    std::unordered_map<uint64_t, OrderLocation> order_lookup;

    void process_list(std::list<Order>& order_list, Order& incoming)
    {
        while (incoming.qty > 0 && !order_list.empty()) {
            Order& resting_order = order_list.front();
            // Q: Why trade std::min(incoming.qty, resting_order.qty) instead of just incoming.qty?
            // A: To allow for partial fills. That way, a resting order can partially fill an incoming one.
            uint32_t traded_qty = std::min(incoming.qty, resting_order.qty);
            incoming.qty -= traded_qty;
            resting_order.qty -= traded_qty;
            total_trades++;
            if (resting_order.qty == 0) {
                // Q: Why erase from order_lookup before calling pop_front()?
                // A: resting_order is a reference into the list node. pop_front() destroys
                //    that node, so accessing resting_order.id afterwards would be undefined
                //    behavior. We read the id first (to do the lookup erase), then destroy
                //    the node with pop_front().
                order_lookup.erase(order_lookup.find(resting_order.id));
                order_list.pop_front();
            }
        }
    }

    void match(Order& incoming)
    {
        if (incoming.is_buy) {
            while (incoming.qty > 0 && !asks.empty()) {
                auto it = asks.begin();
                uint64_t best_ask_price = it->first;
                auto& order_list = it->second;

                // Q: Why break here instead of continue or skip?
                // A: std::map is sorted, so if the order with the best ask price
                //           can't fulfill the order, no order can.
                if (incoming.price < best_ask_price)
                    break;

                process_list(order_list, incoming);

                // Q: Why must we erase the price level from the map when its list becomes empty?
                // A: So we don't try and match incoming orders at a price_level with no resting orders.
                if (order_list.empty())
                    asks.erase(it);
            }
        } else {
            while (incoming.qty > 0 && !bids.empty()) {
                auto it = bids.begin();
                uint64_t best_bid_price = it->first;
                auto& order_list = it->second;

                if (incoming.price > best_bid_price)
                    break;

                process_list(order_list, incoming);

                if (order_list.empty())
                    bids.erase(it);
            }
        }
    }
};
} // namespace Naive
