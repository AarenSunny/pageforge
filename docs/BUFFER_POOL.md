# Buffer pool

The buffer pool keeps a fixed number of disk pages in memory and exposes them
through move-only `PageGuard` objects. A guard pins its frame for its lifetime,
so the page cannot be evicted while a caller holds a reference into it.

## Access contract

```cpp
BufferPool pool(heap, 64);
{
  auto guard = pool.fetch(page_id);
  const auto& readable = guard.page();
  auto& writable = guard.mutable_page(); // marks the frame dirty
}
pool.flush_all();
```

`page()` is read-only. `mutable_page()` marks the frame dirty before returning a
mutable reference, eliminating a common buffer-manager bug where modified pages
are evicted without writeback. Destroying, moving over, or explicitly releasing
the guard decrements the pin count.

## Clock-sweep eviction

Each access sets a reference bit. On a miss, the clock hand:

1. immediately chooses an empty frame;
2. skips pinned frames;
3. clears the reference bit to grant an unpinned page a second chance;
4. evicts the first unpinned frame whose bit is already clear.

A dirty victim is validated and written to the heap before its page-table entry
is removed. If every frame is pinned after two complete sweeps, the request fails
with `BufferPoolExhausted` rather than invalidating a live reference or blocking
forever.

## Durability and metrics

`flush(page_id)` and `flush_all()` propagate write errors and call the heap's
flush boundary. The destructor attempts a best-effort flush but cannot report
errors, so transaction and shutdown code must flush explicitly.

The pool exposes cumulative hit, miss, eviction, and dirty-write counters. These
support the planned benchmark harness and make eviction behavior observable in
tests.

The current buffer manager is single-threaded. Latches and concurrent page-table
access will be added with the transaction layer rather than pretending this
initial API is thread-safe.
