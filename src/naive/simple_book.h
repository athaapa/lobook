#pragma once
#include <cstdint>
#include <iostream>
#include <list>
#include <map>

struct Order {
  uint64_t id;
  uint64_t price;
  uint32_t qty;
  bool is_buy;
};

class NaiveOrderBook {
public:
  uint64_t total_trades = 0;

  void add_order(uint64_t id, uint64_t price, uint32_t qty, bool is_buy) {
    Order ord = {id, price, qty, is_buy};

    match(ord);

    if (ord.qty > 0) {
      if (is_buy) {
        bids[price].push_back(ord);
      } else {
        asks[price].push_back(ord);
      }
    }
  }

private:
  std::map<uint64_t, std::list<Order>, std::greater<uint64_t>> bids;

  std::map<uint64_t, std::list<Order>> asks;

  void process_list(std::list<Order> &order_list, Order &incoming) {
    while (incoming.qty > 0 && !order_list.empty()) {
      Order &resting_order = order_list.front();
      uint32_t traded_qty = std::min(incoming.qty, resting_order.qty);
      incoming.qty -= traded_qty;
      resting_order.qty -= traded_qty;
      total_trades++;
      if (resting_order.qty == 0) {
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
