#ifndef GC_H
#define GC_H

#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cassert>
#include <utility>
#include <typeinfo>
#include <string>

// GC is stop-the-world, not thread-safe
struct GCConfig {
    bool debug = false;
    int logLevel = 1; // 0: None, 1: Summary, 2: Detailed, 3: Full
    size_t poolSize = 1024 * 1024;
};

class GCManager;
class GCWeakPtrBase;

class GCObject {
public:
    bool marked = false;
    int age = 0;
    virtual ~GCObject() = default;
    virtual void trace() {} 
};

struct HeapEntry {
    GCObject* obj;
    size_t size;
    std::string typeName;
    void (*destroy)(void*);
};

class GCManager {
private:
    GCConfig config;

    std::vector<HeapEntry> youngHeap;
    std::vector<HeapEntry> oldHeap;
    
    std::unordered_set<GCObject*> heapSet;
    std::unordered_map<GCObject*, size_t> rootCounts;
    std::unordered_map<GCObject*, std::vector<GCWeakPtrBase*>> weakRefs;
    std::vector<GCObject*> worklist;

    // Memory pool: maps object size to a list of free memory blocks
    std::unordered_map<size_t, std::vector<void*>> freeLists;
    const size_t MAX_POOL_SIZE_PER_TYPE = 1024;

    size_t totalAllocations = 0;
    size_t totalCollections = 0;
    size_t totalMemoryUsage = 0;
    
    std::unordered_map<std::string, size_t> typeCounts;

    GCManager() = default;
    ~GCManager();

public:
    static GCManager& get();

    void setConfig(const GCConfig& cfg) {
        config = cfg;
    }
    
    const GCConfig& getConfig() const { return config; }

    template<typename T>
    static void destroyObj(void* ptr) {
        static_cast<T*>(ptr)->~T();
    }

    template<typename T, typename... Args>
    T* allocate(Args&&... args) {
        size_t sz = sizeof(T);
        void* mem = nullptr;
        
        if (freeLists.count(sz) && !freeLists[sz].empty()) {
            mem = freeLists[sz].back();
            freeLists[sz].pop_back();
        } else {
            mem = ::operator new(sz);
        }
        
        T* obj = new(mem) T(std::forward<Args>(args)...);
        
        youngHeap.push_back({obj, sz, std::string(typeid(T).name()), &destroyObj<T>});
        heapSet.insert(obj);
        
        totalAllocations++;
        totalMemoryUsage += sz;
        typeCounts[std::string(typeid(T).name())]++;

        return obj;
    }

    void addRoot(GCObject* root);
    void removeRoot(GCObject* root);
    
    void addWeakRef(GCObject* obj, GCWeakPtrBase* weakPtr);
    void removeWeakRef(GCObject* obj, GCWeakPtrBase* weakPtr);
    
    void writeBarrier(GCObject* parent, GCObject* child);

    void collect();
    void markObject(GCObject* obj);
    
    size_t getHeapSize() const;
    size_t getRootCount() const;
    bool isHeapObject(GCObject* obj) const;
    
    size_t getTotalAllocations() const { return totalAllocations; }
    size_t getTotalCollections() const { return totalCollections; }
    size_t getMemoryUsage() const { return totalMemoryUsage; }
    
    void printTypeStats() const;
    size_t getMaxPoolSize() const { return MAX_POOL_SIZE_PER_TYPE; }

private:
    void mark();
    void sweep();
};

template<typename T, typename... Args>
T* gc_new(Args&&... args) {
    return GCManager::get().allocate<T>(std::forward<Args>(args)...);
}

void gc_mark(GCObject* obj);

// Write barrier helper for safely mutating object graphs
template<typename P, typename C>
void gc_write(P* parent, C*& field, C* child) {
    field = child;
    GCManager::get().writeBarrier(parent, child);
}

// Smart pointer for strong references (Roots)
template<typename T>
class GCPtr {
private:
    T* ptr;
public:
    GCPtr(T* p = nullptr) : ptr(p) {
        if (ptr) GCManager::get().addRoot(ptr);
    }
    
    ~GCPtr() {
        if (ptr) GCManager::get().removeRoot(ptr);
    }
    
    GCPtr(const GCPtr& other) : ptr(other.ptr) {
        if (ptr) GCManager::get().addRoot(ptr);
    }
    
    GCPtr& operator=(const GCPtr& other) {
        if (this != &other) {
            if (ptr) GCManager::get().removeRoot(ptr);
            ptr = other.ptr;
            if (ptr) GCManager::get().addRoot(ptr);
        }
        return *this;
    }

    GCPtr(GCPtr&& other) noexcept : ptr(other.ptr) {
        other.ptr = nullptr;
    }
    
    GCPtr& operator=(GCPtr&& other) noexcept {
        if (this != &other) {
            if (ptr) GCManager::get().removeRoot(ptr);
            ptr = other.ptr;
            other.ptr = nullptr;
        }
        return *this;
    }

    T& operator*() const { 
        assert(ptr && "Null pointer dereference in GCPtr");
        return *ptr; 
    }
    
    T* operator->() const { 
        assert(ptr && "Null pointer dereference in GCPtr");
        return ptr; 
    }
    
    T* get() const { return ptr; }
    
    void reset(T* p = nullptr) {
        if (ptr == p) return; 
        if (ptr) GCManager::get().removeRoot(ptr);
        ptr = p;
        if (ptr) GCManager::get().addRoot(ptr);
    }
};

template<typename T, typename... Args>
GCPtr<T> make_gc(Args&&... args) {
    return GCPtr<T>(gc_new<T>(std::forward<Args>(args)...));
}

class GCWeakPtrBase {
public:
    virtual void clear() = 0;
    virtual ~GCWeakPtrBase() = default;
};

// Smart pointer for weak references (Does not root)
template<typename T>
class GCWeakPtr : public GCWeakPtrBase {
private:
    T* ptr;
public:
    GCWeakPtr(T* p = nullptr) : ptr(p) {
        if (ptr) GCManager::get().addWeakRef(ptr, this);
    }
    
    ~GCWeakPtr() {
        if (ptr) GCManager::get().removeWeakRef(ptr, this);
    }
    
    GCWeakPtr(const GCWeakPtr& other) : ptr(other.ptr) {
        if (ptr) GCManager::get().addWeakRef(ptr, this);
    }
    
    GCWeakPtr& operator=(const GCWeakPtr& other) {
        if (this != &other) {
            if (ptr) GCManager::get().removeWeakRef(ptr, this);
            ptr = other.ptr;
            if (ptr) GCManager::get().addWeakRef(ptr, this);
        }
        return *this;
    }
    
    GCWeakPtr(const GCPtr<T>& strong) : ptr(strong.get()) {
        if (ptr) GCManager::get().addWeakRef(ptr, this);
    }
    
    GCWeakPtr& operator=(const GCPtr<T>& strong) {
        if (ptr) GCManager::get().removeWeakRef(ptr, this);
        ptr = strong.get();
        if (ptr) GCManager::get().addWeakRef(ptr, this);
        return *this;
    }

    void clear() override { ptr = nullptr; }
    
    T* lock() const { 
        if (ptr && GCManager::get().isHeapObject(ptr)) {
            return ptr;
        }
        return nullptr;
    }
};

#endif // GC_H