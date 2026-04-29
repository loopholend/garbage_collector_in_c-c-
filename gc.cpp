#include "gc.h"
#include <iostream>
#include <chrono>
#include <algorithm>

GCManager::~GCManager() {
    auto cleanupHeap = [&](std::vector<HeapEntry>& heap) {
        for (auto& entry : heap) {
            entry.destroy(entry.obj);
            ::operator delete(entry.obj);
        }
        heap.clear();
    };
    
    cleanupHeap(youngHeap);
    cleanupHeap(oldHeap);
    
    for (auto& pair : freeLists) {
        for (void* mem : pair.second) {
            ::operator delete(mem);
        }
    }
    freeLists.clear();
}

GCManager& GCManager::get() {
    static GCManager instance;
    return instance;
}

void GCManager::addRoot(GCObject* root) {
    if (root) {
        assert(isHeapObject(root) && "Added root must be a valid heap object");
        rootCounts[root]++;
    }
}

void GCManager::removeRoot(GCObject* root) {
    if (root) {
        auto it = rootCounts.find(root);
        if (it != rootCounts.end()) {
            it->second--;
            if (it->second == 0) {
                rootCounts.erase(it);
            }
        }
    }
}

void GCManager::addWeakRef(GCObject* obj, GCWeakPtrBase* weakPtr) {
    if (obj) {
        weakRefs[obj].push_back(weakPtr);
    }
}

void GCManager::removeWeakRef(GCObject* obj, GCWeakPtrBase* weakPtr) {
    if (obj) {
        auto it = weakRefs.find(obj);
        if (it != weakRefs.end()) {
            auto& vec = it->second;
            vec.erase(std::remove(vec.begin(), vec.end(), weakPtr), vec.end());
            if (vec.empty()) {
                weakRefs.erase(it);
            }
        }
    }
}

void GCManager::writeBarrier(GCObject* parent, GCObject* child) {
    // Write barrier hook for future generational remembered sets.
    // If an old object is modified to point to a young object,
    // we would register the parent here to be scanned during a minor GC.
}

void GCManager::collect() {
    auto start = std::chrono::high_resolution_clock::now();
    if (config.debug && config.logLevel >= 1) {
        std::cout << "[GC] Starting collection (Cycle " << totalCollections + 1 << ")...\n";
    }

    assert((youngHeap.size() + oldHeap.size()) == heapSet.size() && "Heap vector and set out of sync");

    mark();
    sweep();
    
    totalCollections++;

    if (config.debug && config.logLevel >= 1) {
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        std::cout << "[GC] Collection finished. Time: " << duration << " ms\n";
    }
}

bool GCManager::isHeapObject(GCObject* obj) const {
    return heapSet.find(obj) != heapSet.end();
}

void GCManager::markObject(GCObject* obj) {
    if (!obj || obj->marked) return;

    if (!isHeapObject(obj)) {
        if (config.debug && config.logLevel >= 1) {
            std::cout << "[GC] WARNING: Attempted to mark non-heap object at " << obj << "\n";
        }
        return;
    }

    obj->marked = true;
    worklist.push_back(obj);

    if (config.debug && config.logLevel >= 3) {
        std::cout << "[GC] Full: Marked object at " << obj << "\n";
    }
}

size_t GCManager::getHeapSize() const { 
    return youngHeap.size() + oldHeap.size(); 
}

size_t GCManager::getRootCount() const {
    return rootCounts.size();
}

void GCManager::printTypeStats() const {
    std::cout << "--- Allocation Stats By Type ---\n";
    for (const auto& pair : typeCounts) {
        std::cout << "Type: " << pair.first << " | Count: " << pair.second << "\n";
    }
    std::cout << "--------------------------------\n";
}

void GCManager::mark() {
    worklist.clear(); 

    for (const auto& pair : rootCounts) {
        assert(pair.first != nullptr && "rootCounts contains null pointer");
        markObject(pair.first);
    }

    while (!worklist.empty()) {
        GCObject* curr = worklist.back();
        worklist.pop_back();
        curr->trace();
    }
}

void GCManager::sweep() {
    size_t localDeletedCount = 0;
    
    auto processHeap = [&](std::vector<HeapEntry>& heap, bool promote) {
        std::vector<HeapEntry> newHeap;
        newHeap.reserve(heap.size());

        for (auto& entry : heap) {
            GCObject* obj = entry.obj;
            if (obj->marked) {
                obj->marked = false;
                obj->age++;
                
                // Promote to old generation after 3 survivals
                if (promote && obj->age > 3) {
                    oldHeap.push_back(entry);
                } else {
                    newHeap.push_back(entry);
                }
            } else {
                if (rootCounts.find(obj) != rootCounts.end()) {
                    if (config.debug && config.logLevel >= 1) {
                        std::cout << "[GC] WARNING: Deleting an object that is still registered as a root at " << obj << "\n";
                    }
                    rootCounts.erase(obj); 
                }
                
                // Clear all associated weak references
                auto weakIt = weakRefs.find(obj);
                if (weakIt != weakRefs.end()) {
                    for (GCWeakPtrBase* weakPtr : weakIt->second) {
                        weakPtr->clear();
                    }
                    weakRefs.erase(weakIt);
                }

                heapSet.erase(obj);
                totalMemoryUsage -= entry.size;
                
                if (typeCounts[entry.typeName] > 0) {
                    typeCounts[entry.typeName]--;
                }

                if (config.debug && config.logLevel >= 3) {
                    std::cout << "[GC] Full: Sweeping (deleting) object at " << obj << "\n";
                }
                
                // Call correct destructor explicitly
                entry.destroy(entry.obj);
                
                // Return memory to pool or delete if pool is full
                if (freeLists[entry.size].size() < getMaxPoolSize()) {
                    freeLists[entry.size].push_back(entry.obj);
                } else {
                    ::operator delete(entry.obj);
                }
                
                localDeletedCount++;
            }
        }
        heap = std::move(newHeap);
    };
    
    processHeap(youngHeap, true);
    processHeap(oldHeap, false);
    
    assert((youngHeap.size() + oldHeap.size()) == heapSet.size() && "Heap and heapSet size mismatch after sweep");
    
    if (config.debug && config.logLevel >= 2) {
        std::cout << "[GC] Detailed: Deleted " << localDeletedCount << " objects in this cycle.\n";
    }
}

void gc_mark(GCObject* obj) {
    GCManager::get().markObject(obj);
}