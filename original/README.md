# High-Performance C++ Matching Engine (LOB)

A low-latency, deterministic Limit Order Book matching engine engineered in modern C++ for high-frequency trading architectures. The engine leverages targeted standard library containers to deliver sub-microsecond trade execution and true O(1) order cancellations.

---

### Benchmark Telemetry
Evaluated under `-O3` compiler optimization on an Apple Silicon processor, executing a stress test of 1,000,000 randomized orders within a compressed bid-ask spread:

* **Throughput:** > 8.19 million operations/second
* **Tick-to-Trade Latency:** ~122 nanoseconds/order

---

### Internal Mechanics & Data Flow

Order processing relies on strict **Price-Time Priority**, partitioned across three coordinated data layers:

* **Price Ladder (O(log N)):** Bids and asks reside in isolated Red-Black trees (`std::map`). The ask book maintains default ascending order, while the bid book uses `std::greater<>` to guarantee instantaneous access to the top-of-book at `begin()`.
* **Execution Queues (FIFO):** Every active price level maintains an independent doubly linked list (`std::list`). Incoming resting limits append to the tail, ensuring strict First-In-First-Out priority.
* **Instant Cancellation (O(1)):** A hash table (`std::unordered_map`) tracks global `OrderID`s to their direct node references (`std::list::iterator`). This decouples order cancellation from depth traversal, enabling surgical pointer splicing in constant time.

---

### Low-Level Optimizations
* **Cache-Conscious Data Layout:** `Order` and `Trade` structs are packed and memory-aligned to minimize CPU cache invalidations.
* **Zero-Copy Matching Path:** Critical execution routines accept orders by constant reference (`const Order&`), eliminating runtime heap allocations during matching sweeps.
* **Comprehensive Fill Logic:** Fully supports maker-taker fee structures and granular partial fills (underfilled, overfilled, and matched balances).

---

### Build & Benchmark Setup

Prerequisites: A C++17 compliant compiler and CMake.

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
./limit_order_book
```
