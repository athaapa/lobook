// Thin shim that forwards a generic book interface to FastOrderBook.
//
// Compiled in its own translation unit so that fast_book.h and
// fast_bitset_book.h (both in `Fast::` namespace, both defining `Order` /
// `PriceLevel`) can coexist in one benchmark binary without ODR conflicts.

#include "../engine/fast_book.h"
#include <cstdint>

namespace FastBookRunner {

static Fast::FastOrderBook book;

void init(size_t max_orders) { book.init(max_orders); }
void submit_order(uint64_t id, uint64_t price, uint32_t qty, bool is_buy) {
    book.submit_order(id, price, qty, is_buy);
}
void cancel_order(uint64_t id) { book.cancel_order(id); }
uint32_t get_quantity(uint64_t id) { return book.get_quantity(id); }

} // namespace FastBookRunner
