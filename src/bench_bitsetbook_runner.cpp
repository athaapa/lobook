#include "engine/fast_bitset_book.h"
#include <cstdint>

// Thin wrappers that forward to FastBitsetOrderBook, compiled in its own TU
// to avoid symbol collisions with fast_book.h.

namespace BitsetBookRunner {

static Fast::FastBitsetOrderBook book;

void init(size_t max_orders) { book.init(max_orders); }
void submit_order(uint64_t id, uint64_t price, uint32_t qty, bool is_buy)
{
    book.submit_order(id, price, qty, is_buy);
}
void cancel_order(uint64_t id) { book.cancel_order(id); }
uint32_t get_quantity(uint64_t id) { return book.get_quantity(id); }

} // namespace BitsetBookRunner
