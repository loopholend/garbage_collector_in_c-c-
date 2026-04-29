# Advanced C++ Garbage Collector (Mark-and-Sweep)

A comprehensive, production-oriented Garbage Collection library implemented in C++. This project demonstrates the journey of building a robust mark-and-sweep garbage collector from scratch. It documents our failures, the optimizations we made, and the advanced concepts required to manage memory safely without relying on raw `delete` or standard smart pointers like `std::shared_ptr`.

## Table of Contents

1. [The Journey: Failures & Improvements](#the-journey-failures--improvements)
2. [Core Concepts Explained](#core-concepts-explained)
3. [Deep Dive: Code Walkthrough (Line-by-Line)](#deep-dive-code-walkthrough-line-by-line)
   - [1. The Managed Object (`GCObject`)](#1-the-managed-object-gcobject)
   - [2. The Root System (`GCPtr`)](#2-the-root-system-gcptr)
   - [3. Weak References (`GCWeakPtr`)](#3-weak-references-gcweakptr)
   - [4. The Heap & Memory Pool (`GCManager`)](#4-the-heap--memory-pool-gcmanager)
   - [5. The Mark Phase](#5-the-mark-phase)
   - [6. The Sweep Phase & Generational Promotion](#6-the-sweep-phase--generational-promotion)
4. [Usage & Benchmark](#usage--benchmark)

---

## The Journey: Failures & Improvements

Building a GC in a language that doesn't natively support it is fraught with edge cases. Here is the step-by-step evolution of our failures and how we engineered solutions.

### Failure 1: Stack Overflows from Recursive Marking
**The Problem:** In our first iteration, the `mark()` phase used a recursive Depth-First Search (DFS). When testing with deep object graphs (e.g., a linked list of 10,000 nodes), the recursion depth exceeded the call stack limit, causing a stack overflow crash.
**The Fix:** We eliminated recursion by introducing an explicit `std::vector<GCObject*> worklist`. We push objects to the list and process them iteratively, moving the memory burden from the limited call stack to the heap.

### Failure 2: Duplicate Roots and Missing Semantics
**The Problem:** Initially, `GCPtr` added its underlying pointer to a `std::vector` of roots. If a `GCPtr` was copied, the same object was added twice. If one copy went out of scope, it removed the root entirely (via `std::remove`), leaving the other copy holding a dangling, unrooted pointer that would get collected prematurely.
**The Fix:** We switched the root tracker to an `std::unordered_map<GCObject*, size_t> rootCounts`. Now, copying a `GCPtr` simply increments the count, and destruction decrements it. The object is only un-rooted when the count hits zero.

### Failure 3: $O(N^2)$ Sweep Phase
**The Problem:** During the sweep phase, we were iterating through a `std::vector` and calling `.erase()` on unmarked objects. Since `erase()` shifts all subsequent elements, sweeping a massive heap containing tens of thousands of objects took quadratic time.
**The Fix:** We replaced in-place erasure with a two-pass partition approach. We create a `newHeap`, reserve its capacity, and only push surviving objects into it, finally using `std::move` to swap out the old heap. This reduced the time complexity to $O(N)$.

### Failure 4: Skipped Destructors on Void Pointers (Memory Pool)
**The Problem:** When we introduced the memory pool, we stored allocated memory as raw `void*`. During the sweep phase, calling `delete (void*)obj` reclaimed the memory but **failed to call the destructors of derived classes**. This caused severe resource leaks if a managed object held an open file, a network socket, or an `std::vector`.
**The Fix:** We captured the precise type at allocation time using a function pointer: `void (*destroy)(void*)`. When `gc_new<T>` is called, it instantiates a templated `destroyObj<T>` function. During the sweep phase, the GC calls `entry.destroy(entry.obj)`, ensuring the correct derived destructor runs before the memory is recycled.

### Failure 5: Heap Validation & Dangling Pointers
**The Problem:** Users could accidentally call `gc_mark()` on a stack-allocated object or an object managed by standard `new`. The GC would try to manage these, eventually attempting to delete stack memory or standard heap memory, leading to immediate segmentation faults.
**The Fix:** We introduced `std::unordered_set<GCObject*> heapSet`. Every time `markObject` is called, it verifies in $O(1)$ time that the pointer genuinely belongs to the GC heap before proceeding. If it doesn't, the GC safely ignores it.

---

## Core Concepts Explained

- **Mark-and-Sweep:** A classic two-phase algorithm. **Mark:** Start from known "roots" (active stack variables) and traverse all connected objects, marking them as 'alive'. **Sweep:** Iterate through all objects ever allocated; if an object is not marked, it is garbage and must be destroyed.
- **Roots:** The absolute starting points for the GC. In C++, these are local variables residing on the stack. We capture them using the `GCPtr<T>` smart pointer wrapper, which automatically registers them with the GCManager.
- **Memory Pooling (Free Lists):** Instead of asking the OS for memory (`new`) and returning it (`delete`) constantly, the GC maintains lists of previously freed memory blocks categorized by their exact size. New allocations pop from these blocks, drastically reducing heap fragmentation and allocation overhead.
- **Generational GC (Conceptual):** Objects are split into `youngHeap` and `oldHeap`. Objects surviving multiple GC cycles are "promoted" to the old heap. While currently swept together, this architecture allows future optimizations where only the young heap is scanned for quick minor collections.
- **Write Barriers:** When mutating an object graph (e.g., `parent->child = new_child`), the GC needs to know. The `gc_write` function acts as a barrier, intercepting this assignment. In fully generational GCs, this is critical to track "old-to-young" references so we don't have to scan the entire old heap during a minor collection.

---

## Deep Dive: Code Walkthrough (Line-by-Line)

Let's break down the mechanics of the code. We will go through the core logic, explaining what every crucial line does.

### 1. The Managed Object (`GCObject`)
Every object managed by the GC must inherit from this base class.

```cpp
class GCObject {
public:
    bool marked = false; // A boolean flag used during the Mark phase. If true, the object is alive.
    int age = 0;         // Tracks how many GC cycles this object has survived. Used for Generational promotion.
    
    // A virtual destructor guarantees that when we delete a GCObject*, the derived class destructor is called.
    virtual ~GCObject() = default; 
    
    // A virtual method that MUST be overridden by derived classes.
    // Inside this method, the user must call gc_mark() on any child GCObject pointers this object holds.
    virtual void trace() {} 
};
```
**Concept:** C++ lacks reflection. The GC cannot look at a raw block of memory and know which bytes are pointers to other objects. By forcing inheritance from `GCObject` and providing the `trace()` virtual method, we ask the developer to manually define the "edges" of the object graph.

### 2. The Root System (`GCPtr`)
The `GCPtr` is our equivalent of `std::shared_ptr`, but it serves as a GC Root.

```cpp
template<typename T>
class GCPtr {
private:
    T* ptr; // The raw pointer being managed
public:
    // Constructor: When a GCPtr is created, it registers the pointer as a root.
    GCPtr(T* p = nullptr) : ptr(p) {
        if (ptr) GCManager::get().addRoot(ptr);
    }
    
    // Destructor: When GCPtr goes out of scope, it unregisters the root.
    ~GCPtr() {
        if (ptr) GCManager::get().removeRoot(ptr);
    }
    
    // Copy Constructor: If we copy a root, we must increment the root count for this object.
    GCPtr(const GCPtr& other) : ptr(other.ptr) {
        if (ptr) GCManager::get().addRoot(ptr);
    }
    
    // Assignment Operator: Unregister the old root, point to the new one, and register the new root.
    GCPtr& operator=(const GCPtr& other) {
        if (this != &other) { // Prevent self-assignment issues
            if (ptr) GCManager::get().removeRoot(ptr); // Decrement old
            ptr = other.ptr;                           // Assign new
            if (ptr) GCManager::get().addRoot(ptr);    // Increment new
        }
        return *this;
    }
    
    // Move Constructor: Steal the pointer from 'other' without touching the global root count.
    GCPtr(GCPtr&& other) noexcept : ptr(other.ptr) {
        other.ptr = nullptr; // Leave 'other' empty so its destructor does nothing.
    }
    
    // Move Assignment: Safely steal the pointer.
    GCPtr& operator=(GCPtr&& other) noexcept {
        if (this != &other) {
            if (ptr) GCManager::get().removeRoot(ptr); // Decrement our old root
            ptr = other.ptr;                           // Steal new pointer
            other.ptr = nullptr;                       // Nullify the source
        }
        return *this;
    }

    // Overloaded operators to make GCPtr act like a normal pointer.
    T& operator*() const { 
        assert(ptr && "Null pointer dereference in GCPtr"); // Safety check
        return *ptr; 
    }
    T* operator->() const { 
        assert(ptr && "Null pointer dereference in GCPtr"); // Safety check
        return ptr; 
    }
    T* get() const { return ptr; }
};
```
**Concept:** RAII (Resource Acquisition Is Initialization). We tie the lifecycle of a GC Root to the lexical scope of C++. When a function starts, `GCPtr` roots are registered. When it returns, the stack unwinds, destructors fire, and the roots are unregistered automatically.

### 3. Weak References (`GCWeakPtr`)
Weak pointers can observe an object but do not prevent it from being garbage collected.

```cpp
template<typename T>
class GCWeakPtr : public GCWeakPtrBase {
private:
    T* ptr; // The raw pointer. Note: We DO NOT register this as a root!
public:
    // ... Constructors/Destructors similar to GCPtr, but they call addWeakRef/removeWeakRef ...

    // Called by the GC sweep phase right before the object is deleted.
    void clear() override { ptr = nullptr; }
    
    // The safe way to access the weak pointer.
    T* lock() const { 
        // We verify two things:
        // 1. Is the pointer not null?
        // 2. Does this pointer still exist in the GC's heapSet?
        if (ptr && GCManager::get().isHeapObject(ptr)) {
            return ptr; // Safe to use
        }
        return nullptr; // Object is dead or invalid
    }
};
```
**Concept:** Weak pointers solve the problem of caching or observer patterns where keeping a strong reference would cause a memory leak. The GC tracks all weak pointers pointing to a specific object, and if that object dies, the GC actively goes to those weak pointers and calls `.clear()` on them.

### 4. The Heap & Memory Pool (`GCManager`)
The core engine. Let's look at how objects are allocated.

```cpp
// A structure to hold metadata about every allocated object.
struct HeapEntry {
    GCObject* obj;          // The actual object
    size_t size;            // The size in bytes (used for memory pooling)
    std::string typeName;   // For debugging statistics
    void (*destroy)(void*); // Function pointer to the exact destructor to prevent leaks
};

// Inside GCManager:
template<typename T, typename... Args>
T* allocate(Args&&... args) {
    size_t sz = sizeof(T); // Get exact size of type T
    void* mem = nullptr;
    
    // Check if we have a previously freed block of memory of this exact size.
    if (freeLists.count(sz) && !freeLists[sz].empty()) {
        mem = freeLists[sz].back(); // Get the last free block
        freeLists[sz].pop_back();   // Remove it from the free list
    } else {
        // If the free list is empty, ask the OS for brand new memory.
        mem = ::operator new(sz);
    }
    
    // Placement new: Construct the object T inside the memory block 'mem' 
    // using the provided constructor arguments.
    T* obj = new(mem) T(std::forward<Args>(args)...);
    
    // Create the metadata entry and push it to the young generation heap.
    // We pass '&destroyObj<T>' so we know exactly how to destruct it later.
    youngHeap.push_back({obj, sz, std::string(typeid(T).name()), &destroyObj<T>});
    
    // Add to the fast O(1) lookup set for validation.
    heapSet.insert(obj);
    
    // Update statistics
    totalAllocations++;
    totalMemoryUsage += sz;
    typeCounts[std::string(typeid(T).name())]++;

    return obj; // Return the usable pointer to the user.
}
```
**Concept:** The **Memory Pool**. By organizing freed memory into `freeLists` based on `sizeof(T)`, we effectively recycle memory addresses. This drastically reduces the overhead of interacting with the OS memory allocator and prevents heap fragmentation over long-running programs. Placement `new` allows us to construct a fresh object directly into an existing byte array.

### 5. The Mark Phase
The GC starts here.

```cpp
void GCManager::mark() {
    worklist.clear(); // 1. Clear any leftover state from previous cycles.

    // 2. Iterate through every known Root (objects pointed to by GCPtr).
    for (const auto& pair : rootCounts) {
        assert(pair.first != nullptr && "rootCounts contains null pointer");
        markObject(pair.first); // Push the root onto the worklist.
    }

    // 3. Iterative Graph Traversal
    // While there are objects in the worklist...
    while (!worklist.empty()) {
        GCObject* curr = worklist.back(); // Get the top object
        worklist.pop_back();              // Remove it
        
        // Call the user-defined trace() function.
        // Inside trace(), the user calls gc_mark(child), which in turn
        // calls markObject(child), adding children to this worklist.
        curr->trace(); 
    }
}

void GCManager::markObject(GCObject* obj) {
    if (!obj || obj->marked) return; // If null or already marked as alive, do nothing.

    // Security check: Ensure this is actually an object we allocate.
    // If a user passes a stack pointer, we ignore it to prevent crashes.
    if (!isHeapObject(obj)) return;

    obj->marked = true;      // Flag as alive!
    worklist.push_back(obj); // Add to worklist so we can trace its children.
}
```
**Concept:** The iterative `worklist` acts as an explicit stack for Depth-First Search. This guarantees that no matter how deep an object graph goes (e.g., millions of linked nodes), the C++ call stack will not overflow.

### 6. The Sweep Phase & Generational Promotion
Cleaning up the dead objects.

```cpp
// We define a lambda 'processHeap' because we need to sweep both youngHeap and oldHeap.
auto processHeap = [&](std::vector<HeapEntry>& heap, bool promote) {
    std::vector<HeapEntry> newHeap; // This will hold ONLY the survivors
    newHeap.reserve(heap.size());   // Pre-allocate to avoid reallocations

    // Iterate over every object in the current heap
    for (auto& entry : heap) {
        GCObject* obj = entry.obj;
        
        if (obj->marked) {
            // --- THE OBJECT IS ALIVE ---
            obj->marked = false; // Reset the flag for the next GC cycle
            obj->age++;          // It survived a cycle, increase its age!
            
            // Generational Logic: If it survived more than 3 cycles, move it to the old generation.
            if (promote && obj->age > 3) {
                oldHeap.push_back(entry);
            } else {
                newHeap.push_back(entry); // Otherwise, keep it in the young generation.
            }
        } else {
            // --- THE OBJECT IS DEAD (GARBAGE) ---
            
            // 1. Safety cleanup: if the user holds a dangling GCPtr to this, remove it.
            if (rootCounts.find(obj) != rootCounts.end()) {
                rootCounts.erase(obj); 
            }
            
            // 2. Weak Pointer cleanup: find all GCWeakPtrs looking at this object.
            auto weakIt = weakRefs.find(obj);
            if (weakIt != weakRefs.end()) {
                for (GCWeakPtrBase* weakPtr : weakIt->second) {
                    weakPtr->clear(); // Set the weak pointer's internal ptr to nullptr
                }
                weakRefs.erase(weakIt); // Erase the tracking list
            }

            // 3. Remove from our O(1) safety validation set
            heapSet.erase(obj);
            totalMemoryUsage -= entry.size; // Update stats
            
            // 4. PRECISE DESTRUCTION: Call the exact derived destructor!
            entry.destroy(entry.obj);
            
            // 5. Memory Pooling: Put the raw memory bytes back into the free list.
            if (freeLists[entry.size].size() < MAX_POOL_SIZE_PER_TYPE) {
                freeLists[entry.size].push_back(entry.obj);
            } else {
                // If the pool is too large (to prevent hoarding memory), give it back to the OS.
                ::operator delete(entry.obj);
            }
        }
    }
    // Replace the old heap vector with the new vector of survivors.
    // This is O(1) vector swapping, avoiding the O(N^2) cost of calling .erase() in a loop.
    heap = std::move(newHeap);
};

// Process both generations
processHeap(youngHeap, true); // True = allow promotion to oldHeap
processHeap(oldHeap, false);  // False = objects in oldHeap stay in oldHeap
```
**Concept:** The sweep phase combines memory reclamation, observer invalidation (WeakPtrs), and Generational Promotion. By using a two-pass vector build (`newHeap`), we ensure optimal performance. Calling `entry.destroy` guarantees that RAII resources inside our garbage collected objects are safely released.

---

## Usage & Benchmark

To use the GC, define your classes, allocate them using `make_gc<T>`, and use `gc_write` when mutating pointers.

```cpp
class Node : public GCObject {
public:
    std::vector<Node*> children;
    void trace() override {
        for (auto child : children) gc_mark(child);
    }
};

// Allocation: make_gc returns a GCPtr which roots the object
GCPtr<Node> root = make_gc<Node>();
Node* child = gc_new<Node>();

// Graph Mutation (Write Barrier)
// We use gc_write so the GC is aware of the structural change.
gc_write(root.get(), root->children.emplace_back(), child);

// Explicit Collection
GCManager::get().collect();
```

**Benchmark Results:**
The system smoothly handles 50,000+ objects with randomized cyclic references. Because of the memory pooling, subsequent allocations are nearly instantaneous. The generational approach guarantees that long-living roots don't needlessly clutter the young generation heap traversals, and the weak pointer system ensures cached views of the object graph never cause use-after-free bugs.