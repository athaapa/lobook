# Non-Obvious Insights

## `book_.init()` must run on the pinned matching thread

`init()` calls `resize()` on the orders pool, which allocates memory. Linux uses a **first-touch policy**: physical memory pages are not allocated when you call `malloc`/`resize` — they're allocated lazily the first time a thread writes to them. Linux places those physical pages on the NUMA node of the thread that first touches them.

On a dual-socket machine (e.g., `attu` — 2x Xeon, each with local DRAM), local memory accesses cost ~70ns and cross-socket accesses cost ~120ns. If `init()` ran on the main thread and the main thread happened to be on the wrong socket, all of the matching engine's order pool accesses would cross the inter-socket interconnect. Every cache miss would pay 50ns extra.

By calling `init()` inside `run()`, after `pin_to_core()`, you guarantee the thread that allocates the memory is the same thread that will use it, pinned to a known core. Linux assigns physical pages to that core's socket. All subsequent accesses are local.

On single-socket machines this makes no measurable difference, but the code is structured to be correct regardless of topology.
