# Minimal C++ Garbage Collector (Mark-and-Sweep)

A lightweight, robust, and extensible Garbage Collection library implemented in C++. This project demonstrates how to build a fully functional, safe, and performant mark-and-sweep garbage collector for C++ applications without relying on external dependencies.

## Table of Contents

- [Evolution & Architecture](#evolution--architecture)
- [Core Features](#core-features)
- [Advanced Enhancements](#advanced-enhancements)
- [Usage](#usage)
- [Design Choices & Benefits](#design-choices--benefits)
- [Testing & Benchmarking](#testing--benchmarking)

## Evolution & Architecture

This project was built iteratively, starting from a basic proof-of-concept and evolving into an advanced, production-oriented GC system. 

### Phase 1: The Core Foundation
Initially, the system was a minimal Mark-and-Sweep implementation. It introduced the `GCObject` base class, a global `GCManager` tracking the heap, and a basic `GCPtr<T>` smart pointer to manage roots.
- **Why:** To establish the fundamental mechanics of garbage collection—identifying reachable objects via roots and reclaiming unreachable memory.
- **Benefit:** Proved the concept of tracking object graphs in C++ without manual `delete`.

### Phase 2: Safety & Robustness
The second iteration focused on hardening the implementation against crashes and stack overflows, particularly under heavy loads (deep object graphs).
- **Iterative Marking:** Replaced recursive DFS marking with an explicit `std::vector` stack (`worklist`). 
  - *Why:* Prevents stack overflows on deep or highly cyclic object graphs.
- **Duplicate Root Protection:** Used `std::unordered_map` for reference counting roots.
  - *Why:* Multiple `GCPtr` instances can point to the same object. Counting ensures a root is only removed when *all* pointers to it go out of scope.
- **Heap Validation:** Introduced `std::unordered_set` to strictly verify heap membership before marking.
  - *Why:* Prevents segmentation faults if a user accidentally attempts to mark a non-GC-allocated object or a dangling pointer.

### Phase 3: Performance & Memory Efficiency
With safety established, the focus shifted to optimizing the allocation/deallocation cycle.
- **Memory Pool Allocator:** Implemented a size-segregated free-list pool.
  - *Why:* Frequent `new` and `delete` calls cause heap fragmentation and are computationally expensive.
  - *Benefit:* Massively speeds up object allocation by reusing previously freed memory blocks.
- **Configurable Logging:** Added `GCConfig` with granular logging levels.
  - *Why:* Console I/O during tight allocation loops causes severe bottlenecks.
  - *Benefit:* Allows users to debug memory leaks effectively when needed, without sacrificing performance in release builds.

### Phase 4: Advanced Runtime Features
The final phase introduced features typical of professional runtime environments (like the JVM or V8).
- **Generational GC (Young/Old Heaps):** Objects that survive multiple GC cycles are promoted to an "Old" heap.
  - *Why:* Follows the generational hypothesis—most objects die young. 
  - *Benefit:* Lays the groundwork for future optimizations where the young generation is scanned more frequently than the old.
- **Weak Pointers (`GCWeakPtr<T>`):** Pointers that do not increment the root count and automatically turn to `nullptr` when the target is collected.
  - *Why:* Essential for implementing caches, listeners, or breaking intentional cycles without leaking memory.
- **Write Barriers (`gc_write`):** A wrapper for assigning pointers between GC objects.
  - *Why:* If an Old object is mutated to point to a Young object, the GC needs to know to scan the Old object during a minor collection.
- **Correct Destructor Dispatch:** Saved the precise destructor function pointer in `HeapEntry`.
  - *Why:* Standard `delete` on a `void*` (from the memory pool) skips derived destructors.
  - *Benefit:* Ensures RAII compliance within GC-managed objects.

## Core Features

- **Mark-and-Sweep Algorithm:** Handles cyclic references flawlessly, a common pitfall for `std::shared_ptr`.
- **Smart Pointer API:** `GCPtr<T>` provides seamless RAII integration, automatically registering and unregistering roots as they enter and leave scope.
- **Stop-The-World:** Operates on a single thread, pausing application logic during the collection phase to ensure graph stability.

## Usage

### 1. Define Managed Objects
Inherit from `GCObject` and override the `trace()` method to tell the GC about child references.

```cpp
class Node : public GCObject {
public:
    Node* child;
    
    void trace() override {
        gc_mark(child); // Tell GC to keep child alive
    }
};
```

### 2. Allocate and Use
Always use `make_gc<T>` or `gc_new<T>` to allocate. Use `GCPtr<T>` to hold strong references on the stack.

```cpp
{
    // Rooted automatically. Will not be collected while in scope.
    GCPtr<Node> root = make_gc<Node>(); 
    
    // Child is kept alive because 'root' points to it.
    gc_write(root.get(), root->child, gc_new<Node>());
}
// 'root' goes out of scope. Both nodes are now eligible for collection.
GCManager::get().collect();
```

## Design Choices & Benefits (Programmer's POV)

1. **Intrusive `GCObject` Base Class:** 
   - *Choice:* Forcing inheritance rather than type-erasure or fat pointers.
   - *Benefit:* Zero overhead for finding the `trace()` function. The virtual table dispatch is highly optimized by modern compilers.
2. **Explicit `trace()` Method:**
   - *Choice:* Manual tracing over reflection or conservative stack scanning.
   - *Benefit:* C++ lacks reflection. Conservative GC (like Boehm) can leak memory by misidentifying integers as pointers. Explicit tracing guarantees 100% precision.
3. **Write Barrier (`gc_write`):**
   - *Choice:* Forcing users to use a helper for assignments.
   - *Benefit:* Prepares the system for advanced concurrent or purely generational collection where inter-generational pointers must be tracked in a remembered set.

## Testing & Benchmarking

The included `main.cpp` runs a rigorous stress test and benchmark:
- Allocates 50,000+ objects with randomized connections (simulating complex, unpredictable data structures).
- Intentionally creates massive cyclic garbage rings.
- Validates that heap size returns to exactly `0` after roots are cleared, proving zero memory leaks.
- Measures and outputs allocation times, total memory footprint, and object-type statistics.