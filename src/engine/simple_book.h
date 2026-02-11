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

struct OrderLocation {
    uint64_t price;
    bool is_buy;
    std::list<Order>::iterator itr;
};

class NaiveOrderBook {
  public:
    int total_trades{};

    void add_order(uint64_t id, uint64_t price, uint32_t qty, bool is_buy) {
        Order ord = {id, price, qty, is_buy};

        match(ord);

        if (ord.qty > 0) {
            auto &list = is_buy ? bids[price] : asks[price];
            auto it = list.insert(list.end(), ord);
            order_lookup[id] = {price, is_buy, it};
        }
    }

    void cancel_order(uint64_t id) {
        auto it_lookup = order_lookup.find(id);
        if (it_lookup != order_lookup.end()) {
            OrderLocation &loc = it_lookup->second;
            if (loc.is_buy) {
                bids[loc.price].erase(loc.itr);
            } else {
                asks[loc.price].erase(loc.itr);
            }
            order_lookup.erase(it_lookup);
        }
    }

    bool has_order(uint64_t id) { return order_lookup.count(id); }

    void debug_print() {
        std::cout << "--- BOOK STATE ---\n";
        for (auto const &[price, list] : bids) {
            std::cout << "Bid Price " << price << " has " << list.size()
                      << " orders.\n";
        }
    }

  private:
    std::map<uint64_t, std::list<Order>, std::greater<uint64_t>> bids;
    std::map<uint64_t, std::list<Order>> asks;
    std::unordered_map<uint64_t, OrderLocation> order_lookup;

    void process_list(std::list<Order> &order_list, Order &incoming) {
        while (incoming.qty > 0 && !order_list.empty()) {
            Order &resting_order = order_list.front();
            uint32_t traded_qty = std::min(incoming.qty, resting_order.qty);
            incoming.qty -= traded_qty;
            resting_order.qty -= traded_qty;
            total_trades++;
            if (resting_order.qty == 0) {
                order_lookup.erase(order_lookup.find(resting_order.id));
                order_list.pop_front();
            }
        }
    }

    void match(Order &incoming) {
        if (incoming.is_buy) {
            while (incoming.qty > 0 && !asks.empty()) {
                auto it = asks.begin();
                uint64_t best_ask_price = it->first;
                auto &order_list = it->second;

                if (incoming.price < best_ask_price)
                    break;

                process_list(order_list, incoming);

                if (order_list.empty())
                    asks.erase(it);
            }
        } else {
            while (incoming.qty > 0 && !bids.empty()) {
                auto it = bids.begin();
                uint64_t best_bid_price = it->first;
                auto &order_list = it->second;

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
