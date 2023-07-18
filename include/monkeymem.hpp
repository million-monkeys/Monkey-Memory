#ifndef _MONKEY_MEMORY__H_
#define _MONKEY_MEMORY__H_

#include <cstdint>
#include <cstddef>
#include <functional>
#include <new>
#include <stdexcept>
#include <atomic>
#include <thread>
#include <vector>

#ifdef MONKEYMEM_OMIT_ALL_SAFETY_CHECKS
#   define MONKEYMEM_OMIT_BUFFER_RANGE_CHECKS
#   define MONKEYMEM_OMIT_POOL_RANGE_CHECKS
#   define MONKEYMEM_OMIT_OUTOFMEMORY_CHECKS
#endif

#if defined(__GNUC__) || defined(__clang__) || defined(__GNUG__)
#   ifndef EXPECT_NOT_TAKEN
#       define EXPECT_NOT_TAKEN(cond) (__builtin_expect(int(cond), 0))
#   endif
#   ifndef EXPECT_TAKEN
#       define EXPECT_TAKEN(cond) (__builtin_expect(int(cond), 1))
#   endif
#else
#   ifndef EXPECT_NOT_TAKEN
#       define EXPECT_NOT_TAKEN(cond) (cond)
#   endif
#   ifndef EXPECT_TAKEN
#       define EXPECT_TAKEN(cond) (cond)
#   endif
#endif


namespace monkeymem {

    class Buffer;
    struct EarlyExit {};


    #ifndef ERROR_LOG_FUNCTION
        namespace log {
            template <typename... Args> void error (Args... args) {}
        }
        #define ERROR_LOG_FUNCTION log::error
    #endif

    namespace helpers {
        template <typename T>
        T* align(T* pointer, const uintptr_t bytes_alignment) {
            intptr_t value = reinterpret_cast<intptr_t>(pointer);
            value += (-value) & (bytes_alignment - 1);
            return reinterpret_cast<T*>(value);
        }

        #if __cpp_lib_hardware_interference_size >= 201603
            constexpr unsigned CACHELINE_SIZE = std::hardware_destructive_interference_size;
        #else
            constexpr unsigned CACHELINE_SIZE = 64;
        #endif
    }

    namespace units {
        template <typename T> constexpr T kilobytes (T count) { return 1024 * count; }
        template <typename T> constexpr T megabytes (T count) { return 1024 * kilobytes(count); }
        template <typename T> constexpr T cachelines (T count) { return helpers::CACHELINE_SIZE * count; }
    }

    namespace alignment {
        struct NoAlign {
            static constexpr int Boundary = 1;
            static constexpr std::uint32_t adjust_size (std::uint32_t size) { return size; }
            template <typename T> static T* align (void* buffer) {
                return reinterpret_cast<T*>(buffer);
            }
            static constexpr std::uint32_t fit_multiple (std::uint32_t size) { return size; }
        };

        template <int BoundaryT>
        struct Aligned {
            static constexpr int Boundary = BoundaryT;
            static constexpr std::uint32_t adjust_size (std::uint32_t size) { return size + BoundaryT; }
            template <typename T> static T* align (void* buffer) {
                return reinterpret_cast<T*>(helpers::align(buffer, BoundaryT));
            }
            static constexpr std::uint32_t fit_multiple (std::uint32_t size) {
                std::uint32_t multiple = BoundaryT;
                while (size > multiple) {
                    multiple += BoundaryT;
                }
                return multiple;
            }
        };

        using AlignCacheLine = Aligned<helpers::CACHELINE_SIZE>;
        using AlignSIMD= Aligned<16>;
    }

    namespace out_of_space_policies {
        struct Throw {
            template <typename T>
            static T* apply (const std::string& name) {
                throw std::runtime_error(name + " allocated more items than reserved space");
            }
        };

        struct Log {
            template <typename T>
            static T* apply (const std::string& name) {
                ERROR_LOG_FUNCTION("Allocated more items than reserved space", name);
                return nullptr;
            }
        };

        struct Ignore {
            template <typename T>
            static T* apply (const std::string& name) {
                return nullptr;
            }
        };
    }

    namespace concurrency_policies {
        struct Unsafe {
            struct StackAllocatorPolicy {
            public:
                StackAllocatorPolicy () : next(0) {}
                std::uint32_t fetch () const {
                    return next;
                }
                std::uint32_t fetch_add (std::uint32_t amount) {
                    auto cur = next;
                    next += amount;
                    return cur;
                }
                void put (std::uint32_t value) {
                    next = value;
                }
                template <typename Condition, typename AtomicOnlyCallback, typename Callback>
                void synced (Condition, AtomicOnlyCallback, Callback callback) const {
                    callback(); // Condition & AtomicOnlyCallback are optimized out in non-atomic version
                }
            private:
                std::uint32_t next;
            };

            struct BlockAllocatorPolicy {

            };
        };

        struct Atomic {
            struct StackAllocatorPolicy {
            public:
                StackAllocatorPolicy () : next(0) {}
                std::uint32_t fetch () const {
                    return next.load();
                }
                std::uint32_t fetch_add (std::uint32_t amount) {
                    return next.fetch_add(amount);
                }
                void put (std::uint32_t value) {
                    next.store(value);
                }
                template <typename Condition, typename AtomicOnlyCallback, typename Callback>
                void synced (Condition condition, AtomicOnlyCallback ao_callback, Callback callback) const {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    if (condition()) {
                        ao_callback();
                        callback();
                    }
                }
            private:
                std::atomic_uint32_t next;
                mutable std::mutex m_mutex;
            };
    
            struct BlockAllocatorPolicy {

            };
        };
    }

    class MemoryAllocator {
    public:
        virtual std::byte* allocate (std::size_t bytes, std::size_t alignment) = 0;
        virtual void release (std::byte* ptr, std::size_t size) = 0;
    };

    template <typename Alloc, typename Dealloc>
    class MemoryAllocatorWrapper : public MemoryAllocator {
    public:
        MemoryAllocatorWrapper (Alloc alloc, Dealloc dealloc) : m_allocate(alloc), m_deallocate(dealloc) {}
        ~MemoryAllocatorWrapper() {}

        std::byte* allocate (std::size_t bytes, std::size_t alignment=1) final {
            // Calculate space for offset header
            std::size_t header = 0;
            if (alignment < 4) {
                header = 4;
            }
            // Allocate raw memory
            std::size_t amount = bytes + alignment + header;
            std::byte* memory = m_allocate(amount);
            // Align pointer
            std::byte* ptr = helpers::align(memory + header, alignment);
            // Save offset and size
            std::uint16_t offset = std::uint16_t(ptr - memory);
            reinterpret_cast<std::uint16_t*>(ptr)[-1] = offset;
            reinterpret_cast<std::uint16_t*>(ptr)[-2] = amount;
            // Return aligned pointer
            return ptr;
        }

        void release (std::byte* ptr, std::size_t) final {
            // Retrieve offset and size
            std::uint16_t offset = reinterpret_cast<std::uint16_t*>(ptr)[-1];
            std::uint16_t size = reinterpret_cast<std::uint16_t*>(ptr)[-2];
            // Deallocate raw memory
            m_deallocate(ptr - offset, size);
        }

    private:
        Alloc m_allocate;
        Dealloc m_deallocate;
    };

    class Buffer {
    public:
        static Buffer create (MemoryAllocator& allocator, std::size_t size, std::uint32_t alignment=1) {
            auto ptr = allocator.allocate(size, alignment);
            return {
                size,
                ptr,
            };
        }

        static void destroy (MemoryAllocator& allocator, Buffer& buffer) {
            if (buffer.m_memory) {
                allocator.release(buffer.m_memory, buffer.m_size);
            }
            buffer.m_size = 0;
            buffer.m_memory = nullptr;
        }

        Buffer() : m_memory(nullptr), m_next(nullptr), m_size(0), m_end(0) {}
        Buffer (std::size_t size, std::byte* memory) : m_memory(memory), m_next(nullptr), m_size(size), m_end(size) {}
        Buffer (std::size_t size, std::byte* memory, Buffer* next) : m_memory(memory), m_next(next), m_size(size), m_end(size) {}
        Buffer (Buffer&& other) : m_memory(other.m_memory), m_next(other.m_next), m_size(other.m_size), m_end(other.m_end) {
            other.m_memory = nullptr;
            other.m_next = 0;
            other.m_size = 0;
            other.m_end = 0;
        }
        ~Buffer () {}

        void operator= (Buffer&& other) {
            m_size = other.m_size;
            m_memory = other.m_memory;
            m_next = other.m_next;
            other.m_size = 0;
            other.m_memory = nullptr;
        }

        void operator= (const Buffer& other) {
            m_size = other.m_size;
            m_memory = other.m_memory;
            m_next = other.m_next;
        }

        bool operator== (const Buffer& other) {
            return m_memory == other.m_memory;
        }

        bool valid () const {
            return m_memory != nullptr;
        }

        std::size_t capacity () const {
            return m_size;
        }

        std::size_t size () const {
            return m_end;
        }

        std::byte* data () const {
            return m_memory;
        }

        std::byte* begin () const {
            return m_memory;
        }

        std::byte* end () const {
            return m_memory + m_end;
        }

        std::byte& operator[] (std::size_t index) {
            #ifndef MONKEYMEM_OMIT_BUFFER_RANGE_CHECKS
            if EXPECT_TAKEN(index < m_end) {
            #endif
                return m_memory[index];
            #ifndef MONKEYMEM_OMIT_BUFFER_RANGE_CHECKS
            } else {
                throw std::out_of_range("Buffer index out of range");
            }
            #endif
        }

        Buffer view (std::size_t start_index, std::size_t size, bool include_next=false) {
            if (start_index + size <= m_end) {
                return Buffer{size, m_memory + start_index, include_next ? m_next : nullptr };
            } else {
                return Buffer{};
            }
        }

        Buffer view (std::size_t start_index, bool include_next=false) {
            return view(start_index, m_end - start_index, include_next);
        }

        Buffer view (bool include_next=false) {
            return view(0, m_end, include_next);
        }

        // Set the end of the buffer
        void end (std::size_t e) {
            #ifndef MONKEYMEM_OMIT_BUFFER_RANGE_CHECKS
            if EXPECT_NOT_TAKEN(e > m_size) {
                throw std::out_of_range("Buffer end cannot be greater than size");
            }
            #endif
            m_end = e;
        }

        // Buffer linkage

        Buffer* next () const {
            return m_next;
        }

        void next (Buffer* next_buffer) {
            m_next = next_buffer;
        }

        // Reset buffer end and linkage
        void reset () {
            m_next = nullptr;
            m_end = m_size;
        }

        // Walk through this and each linked buffer
        template <typename Func>
        void walk (Func func) {
            const Buffer* cur = this;
            do {
                func(*cur);
                cur = cur->next();
            } while (cur != nullptr);
        }
        template <typename Func>
        void walk (EarlyExit, Func func) {
            const Buffer* cur = this;
            do {
                if (! func(*cur)) {
                    break;
                }
                cur = cur->next();
            } while (cur != nullptr);
        }

    private:
        std::byte*  m_memory;
        Buffer* m_next; // Buffers support linking
        std::uint16_t m_size;
        std::uint16_t m_end;
    };

    namespace buffer_pools {
        template <typename Alignment=alignment::NoAlign>
        class AtomicStack {
        public:
            static constexpr int AlignmentBoundary = Alignment::Boundary;

            AtomicStack (MemoryAllocator& allocator, std::size_t count, std::size_t buffer_size) : m_allocator(allocator), m_next(0) {
                m_buffers.reserve(count);
                for (auto i=0; i<count; ++i) {
                    auto& buf = m_buffers.emplace_back(Buffer::create(m_allocator, buffer_size, AlignmentBoundary));
                }
            }
            AtomicStack (AtomicStack&& other) : m_static_buffers(std::move(other.m_static_buffers)), m_buffers(std::move(other.m_buffers)), m_allocator(other.m_allocator), m_next(other.m_next) {
                other.m_next = 0;
            }
            ~AtomicStack () {
                clear();
            }

            std::size_t capacity () const {
                return m_buffers.size();
            }

            std::size_t used () const {
                return m_next.load();
            }

            std::size_t available () const {
                return capacity() - used();
            }

            template <typename OutOfSpacePolicy=out_of_space_policies::Throw>
            Buffer* allocate () {
                auto index = m_next.fetch_add(1);
                #ifndef MONKEYMEM_OMIT_OUTOFMEMORY_CHECKS
                if EXPECT_TAKEN(index < m_buffers.size()) {
                #endif
                    return &m_buffers[index];
                #ifndef MONKEYMEM_OMIT_OUTOFMEMORY_CHECKS
                } else {
                    return OutOfSpacePolicy::template apply<Buffer>("buffer_pools::AtomicStack");
                }
                #endif
            }

            void deallocate (Buffer*) { /* No Op */ }

            void reset () {
                auto last = m_next.load();
                for (auto i=0; i<last; ++i) {
                    m_buffers[i].reset();
                }
                m_next.store(0);
            }

            void clear () {
                for (auto& buffer : m_buffers) {
                    Buffer::destroy(m_allocator, buffer);
                }
                m_buffers.clear();
                for (auto& buffer : m_static_buffers) {
                    Buffer::destroy(m_allocator, buffer);
                }
                m_static_buffers.clear();
            }

            Buffer* allocate_static (std::size_t buffer_size) {
                return &m_static_buffers.emplace_back(Buffer::create(m_allocator, buffer_size, AlignmentBoundary));
            }

        private:
            std::vector<Buffer> m_static_buffers;
            std::vector<Buffer> m_buffers;
            MemoryAllocator& m_allocator;
            std::atomic_uint32_t m_next;
        };

        template <typename Alignment=alignment::NoAlign>
        class FreeList {
        public:
            static constexpr int AlignmentBoundary = Alignment::Boundary;

            FreeList (MemoryAllocator& allocator, std::size_t count, std::size_t buffer_size) : m_allocator(allocator) {
                m_buffers.reserve(count);
                m_free_list.reserve(count);
                for (auto i=0; i<count; ++i) {
                    auto& buf = m_buffers.emplace_back(Buffer::create(m_allocator, buffer_size, AlignmentBoundary));
                    m_free_list.push_back(&buf);
                }            }
            FreeList (FreeList&& other) : m_static_buffers(std::move(other.m_static_buffers)), m_buffers(std::move(other.m_buffers)), m_free_list(std::move(other.m_free_list)), m_allocator(other.m_allocator) {}
            ~FreeList () {
                clear();
            }

            std::size_t capacity () const {
                return m_buffers.size();
            }

            std::size_t used () const {
                return capacity() - available();
            }

            std::size_t available () const {
                return m_free_list.size();
            }

            template <typename OutOfSpacePolicy=out_of_space_policies::Throw>
            Buffer* allocate () {
                #ifndef MONKEYMEM_OMIT_OUTOFMEMORY_CHECKS
                if EXPECT_TAKEN(m_free_list.size()) {
                #endif
                    auto buffer = m_free_list.back();
                    m_free_list.pop_back();
                    return buffer;
                #ifndef MONKEYMEM_OMIT_OUTOFMEMORY_CHECKS
                } else {
                    return OutOfSpacePolicy::template apply<Buffer>("buffer_pools::FreeList");
                }
                #endif
            }

            void deallocate (Buffer* buffer) {
                Buffer* curr = buffer;
                while (curr != nullptr) {
                    m_free_list.push_back(curr);
                    auto next = curr->next();
                    curr->reset();
                    curr = next;
                }
            }

            void reset () {
                m_free_list.clear();
                for (auto& buffer : m_buffers) {
                    buffer.reset();
                    m_free_list.push_back(&buffer);
                }
            }

            void clear () {
                for (auto& buffer : m_buffers) {
                    Buffer::destroy(m_allocator, buffer);
                }
                m_buffers.clear();
                m_free_list.clear();
                for (auto& buffer : m_static_buffers) {
                    Buffer::destroy(m_allocator, buffer);
                }
                m_static_buffers.clear();
            }

            Buffer* allocate_static (std::size_t buffer_size) {
                return &m_static_buffers.emplace_back(Buffer::create(m_allocator, buffer_size, AlignmentBoundary));
            }

        private:
            std::vector<Buffer> m_static_buffers;
            std::vector<Buffer> m_buffers;
            std::vector<Buffer*> m_free_list;
            MemoryAllocator& m_allocator;
        };
    }

    namespace data_access {
        template <typename T>
        struct Iterator {
            using iterator_category = std::forward_iterator_tag;
            using difference_type   = std::ptrdiff_t;
            using value_type        = T;
            using pointer           = value_type*;
            using reference         = value_type&;

            Iterator (pointer ptr) : m_ptr(reinterpret_cast<std::byte*>(ptr)) {}
            Iterator (std::byte* ptr) : m_ptr(ptr) {}
            Iterator (const Iterator& other) : m_ptr(other.m_ptr) {}
            Iterator (Iterator&& other) : m_ptr(other.m_ptr) { other.m_ptr = nullptr; }

            reference operator*() const {
                return *reinterpret_cast<pointer>(m_ptr);
            }

            pointer operator->() {
                return reinterpret_cast<pointer>(m_ptr);
            }

            // Prefix increment
            Iterator& operator++() {
                m_ptr += sizeof(value_type);
                return *this;
            }

            // Postfix increment
            Iterator operator++(int) {
                Iterator tmp = *this;
                ++(*this);
                return tmp;
            }

            friend bool operator== (const Iterator& a, const Iterator& b) { return a.m_ptr == b.m_ptr; };
            friend bool operator!= (const Iterator& a, const Iterator& b) { return a.m_ptr != b.m_ptr; };     

        private:
            std::byte* m_ptr;
        };

        template <typename T>
        struct Iterable {
            using iterator = Iterator<T>;
            Iterable (Buffer* buffer) : m_buffer(buffer) {}
            iterator begin () const { return iterator{m_buffer->begin()};}
            iterator end () const { return iterator{m_buffer->end()};}

        private:
            Buffer* m_buffer;
        };

        template <typename T>
        struct PagedIterable {
            using iterator = Iterator<T>;
            PagedIterable (Buffer* buffer) : m_buffer(buffer) {}
            iterator begin () const { return iterator{m_buffer->begin()};}
            iterator end () const { return iterator{m_buffer->end()};}

            bool next () {
                m_buffer = m_buffer->next();
                return m_buffer != nullptr;
            }
        private:
            Buffer* m_buffer;
        };

        template <typename T>
        struct ConstPagedIterable {
            using iterator = Iterator<const T>;
            ConstPagedIterable (const Buffer* buffer) : m_buffer(buffer) {}
            iterator begin () const { return iterator{m_buffer->begin()};}
            iterator end () const { return iterator{m_buffer->end()};}

            bool next () {
                m_buffer = m_buffer->next();
                return m_buffer != nullptr;
            }
        private:
            const Buffer* m_buffer;
        };

        template <typename T, typename Func>
        void for_each (const Buffer* buffer, Func func) {
            auto iter = PagedIterable<T>(buffer);
            do {
                for (auto& it : iter) {
                    func(*it);
                }
            } while (iter->next());
        }

        template <typename T, typename Func>
        void const_for_each (const Buffer* const buffer, Func func) {
            auto iter = ConstPagedIterable<T>(buffer);
            do {
                for (const auto& it : iter) {
                    func(*it);
                }
            } while (iter->next());
        }
    }

    namespace default_policies {
        using Concurrency = concurrency_policies::Unsafe;
        using ItemAlign = alignment::NoAlign;
        using OutOfSpace = out_of_space_policies::Throw;
    }

    template <typename ConcurrencyPolicy = default_policies::Concurrency, typename ItemAlignPolicy = default_policies::ItemAlign, typename OutOfSpacePolicy = default_policies::OutOfSpace>
    struct Policies {
        using Concurrency = ConcurrencyPolicy;
        using ItemAlign = ItemAlignPolicy;
        using OutOfSpace = OutOfSpacePolicy;
    };

    namespace heterogeneous {

        // A basic stack allocator. Objects can be allocated from the top of the stack, but are deallocated all at once. Pointers to elements are stable until reset() is called.
        template <typename BufferPool, typename Policies=Policies<>>
        class StackPool {
        public:
            using ConcurrencyPolicyType = typename Policies::Concurrency::StackAllocatorPolicy;
            using ItemAlignPolicyType = typename Policies::ItemAlign;
            using OutOfSpacePolicyType = typename Policies::OutOfSpace;

            StackPool (BufferPool& buffers, std::size_t size) :
                m_first(buffers.allocate_static(size)),
                m_current(m_first),
                m_buffers(buffers)
            {}
            StackPool (StackPool&& other) :
                m_first(other.m_first),
                m_current(other.m_current),
                m_buffers(other.m_buffers)
            {
                other.m_first = nullptr;
                other.m_current = nullptr;
            }
            ~StackPool() {}

            // Allocate, but don't construct
            std::byte* unaligned_allocate (std::uint32_t bytes) {
                auto offset = m_concurrency_policy.fetch_add(bytes);
                if (offset + bytes > m_current->size()) {
                    m_concurrency_policy.synced(
                        // Condition to decide whether to run the update, only needed in atomic version
                        [this, &offset, bytes](){
                            // Used to recheck, but also for the side effect of setting offset
                            offset = m_concurrency_policy.fetch_add(bytes);
                            // Check whether the buffers still need updating
                            return offset + bytes > m_current->size();
                        },
                        // Only run in atomic version and only if previous lambda returns true
                        [&offset, bytes](){
                            offset -= bytes; // Remove the effect of the extra fetch_add added by running first lambda in atomic version
                        },
                        // Update the buffers, only if first lambda returns true
                        [this, &offset, bytes](){
                            // The first thread to reach the synced block must allocate a new buffer
                            auto* buffer = m_buffers.template allocate<OutOfSpacePolicyType>();
                            // Set the end of the buffer
                            m_current->end(offset);
                            // Link new buffer into chain
                            m_current->next(buffer);
                            // Set new buffer as current
                            m_current = buffer;
                            // Allocate bytes in new buffer
                            m_concurrency_policy.put(bytes);
                            // Set the offset to the start of the buffer
                            offset = 0;
                        }
                    );
                    #ifndef MONKEYMEM_OMIT_OUTOFMEMORY_CHECKS
                    if EXPECT_NOT_TAKEN(m_current == nullptr) {
                        return nullptr;
                    }
                    #endif
                }
                return m_current->data() + offset;
            }

            std::byte* allocate (std::uint32_t bytes) {
                return ItemAlignPolicyType::template align<std::byte>(unaligned_allocate(ItemAlignPolicyType::adjust_size(bytes)));
            }
                
            // Allocate and construct
            template <typename T, typename... Args>
            T* emplace (Args&&... args) {
                static_assert(std::is_trivial_v<T>, "StackPool<T>::emplace(Args...) T must be trivial");
                auto ptr = alloc<T>();
                #ifndef MONKEYMEM_OMIT_OUTOFMEMORY_CHECKS
                if EXPECT_NOT_TAKEN(!ptr) {
                    return nullptr;
                }
                #endif
                return new(ptr) T{std::forward<Args>(args)...};
            }

            template <typename T>
            T* push_back (const T& item) {
                static_assert(std::is_trivial_v<T>, "StackPool<T>::push_back(const T&) T must be trivial");
                auto ptr = alloc<T>();
                #ifndef MONKEYMEM_OMIT_OUTOFMEMORY_CHECKS
                if EXPECT_NOT_TAKEN(!ptr) {
                    return nullptr;
                }
                #endif
                return new(ptr) T{item};
            }

            template <typename T>
            T* push_back (T&& item) {
                static_assert(std::is_trivial_v<T>, "StackPool<T>::push_back(T&&) T must be trivial");
                auto ptr = alloc<T>();
                #ifndef MONKEYMEM_OMIT_OUTOFMEMORY_CHECKS
                if EXPECT_NOT_TAKEN(!ptr) {
                    return nullptr;
                }
                #endif
                return new(ptr) T{std::move(item)};
            }

            void reset () {
                m_concurrency_policy.synced(
                    [](){ return true; },
                    [](){},
                    [this](){
                        ConcurrencyPolicyType::put(0);
                        m_buffers.deallocate(m_first->next());
                        m_first->reset();
                        m_current = m_first;
                    }
                );
            }

            // Access underlying data
            Buffer* raw () const {
                // The current size of the latest buffer must be recorded
                m_concurrency_policy.synced(
                    [](){ return true; },
                    [](){},
                    [this](){
                        m_current->end(m_concurrency_policy.fetch());
                    }
                );
                // Then the first buffer is returned
                return m_first;
            }

            Buffer data () const {
                return raw()->view(true);
            }


        private:
            Buffer* m_first;
            Buffer* m_current;
            BufferPool& m_buffers;
            ConcurrencyPolicyType m_concurrency_policy;

            template <typename T> T* alloc () {
                return ItemAlignPolicyType::template align<T>(unaligned_allocate(ItemAlignPolicyType::adjust_size(sizeof(T))));
            }
        };

    }

    namespace homogeneous {

        // A basic stack allocator. Objects can be allocated from the top of the stack, but are deallocated all at once. Pointers to elements are stable until reset() is called.
        template <typename T, typename BufferPool, typename Policies=Policies<>>
        class StackPool {
        public:
            using ConcurrencyPolicyType = typename Policies::Concurrency::StackAllocatorPolicy;
            using ItemAlignPolicyType = typename Policies::ItemAlign;
            using OutOfSpacePolicyType = typename Policies::OutOfSpace;
            using iterable = data_access::PagedIterable<T>;
            using const_iterable = data_access::PagedIterable<const T>;

            // Ctor taking unused T to allow template type inference
            StackPool (T, BufferPool& buffers, std::size_t size) : StackPool(buffers, size) {}
            // Normal ctors
            StackPool (BufferPool& buffers, std::size_t size) :
                m_impl(buffers, size)
            {}
            StackPool (StackPool&& other) :
                m_impl(std::move(other.m_impl))
            {}
            ~StackPool() {}

            template <typename... Args>
            T* emplace (Args&&... args) {
                return m_impl.template emplace<T>(args...);
            }

            T* push_back (const T& item) {
                return m_impl.push_back(item);
            }

            T* push_back (T&& item) {
                return m_impl.push_back(std::move(item));
            }
            
            void reset () {
                if constexpr (! std::is_trivial<T>::value) {
                    // Not a trivial type, so need to call the constructor
                    each([](auto& it){
                        it.~T();
                    });
                }
                m_impl.reset();
            }

            iterable iter () const {
                return iterable(m_impl.raw());
            }
            const_iterable const_iter () const {
                return iterable(m_impl.raw());
            }

            template<typename Func> void each (Func func) const {
                auto it = iter();
                do {
                    // Enforce constness of underlying if pool is const
                    if constexpr (std::is_const_v<decltype(this)>) {
                        for (const auto& item : it) {
                            func(item);
                        }
                    } else {
                        for (auto& item : it) {
                            func(item);
                        }
                    }
                } while (it.next());
            }

        private:
            heterogeneous::StackPool<BufferPool, Policies> m_impl;
        };

        namespace block_pools {

            template <typename T, typename BufferPool, typename Policies=Policies<>>
            class FreeList {
                static_assert(std::is_trivial<T>::value, "homogeneous::block_pools::FreeList<T> must contain a trivial type T");
            public:
                using value_type = T;
                using ConcurrencyPolicyType = typename Policies::Concurrency::BlockAllocatorPolicy;
                using ItemAlignPolicyType = typename Policies::ItemAlign;
                using OutOfSpacePolicyType = typename Policies::OutOfSpace;
                using iterable = data_access::PagedIterable<T>;
                using const_iterable = data_access::PagedIterable<const T>;

                static constexpr std::size_t block_size = ItemAlignPolicyType::fit_multiple(sizeof(T));

                // Ctor taking unused T to allow template type inference
                FreeList (T, BufferPool& buffers, std::size_t size) : FreeList(buffers, size) {}
                // Normal ctors
                FreeList (BufferPool& buffers, std::size_t size) :
                    m_first(buffers.allocate_static(size)),
                    m_current(m_first),
                    m_buffers(buffers)
                {
                    reset();
                }
                FreeList (FreeList&& other) :
                    m_first(other.m_first),
                    m_current(other.m_current),
                    m_buffers(other.m_buffers)
                {
                    other.m_first = nullptr;
                    other.m_current = nullptr;
                }
                ~FreeList() {}
    
                template <typename... Args>
                [[nodiscard]] T* emplace (Args&&... args) {
                    auto ptr = allocate();
                    #ifndef MONKEYMEM_OMIT_OUTOFMEMORY_CHECKS
                    if EXPECT_NOT_TAKEN(!ptr) {
                        return nullptr;
                    }
                    #endif
                    return new(ptr) T{args...};
                }

                [[nodiscard]] T* push_back (const T& item) {
                    auto ptr = allocate();
                    #ifndef MONKEYMEM_OMIT_OUTOFMEMORY_CHECKS
                    if EXPECT_NOT_TAKEN(!ptr) {
                        return nullptr;
                    }
                    #endif
                    return new(ptr) T{item};
                }

                [[nodiscard]] T* push_back (T&& item) {
                    auto ptr = allocate();
                    #ifndef MONKEYMEM_OMIT_OUTOFMEMORY_CHECKS
                    if EXPECT_NOT_TAKEN(!ptr) {
                        return nullptr;
                    }
                    #endif
                    return new(ptr) T{std::move(item)};
                }

                void discard (T* object) const {
                    const std::byte* addr = reinterpret_cast<std::byte*>(object);
                    #ifndef MONKEYMEM_OMIT_POOL_RANGE_CHECKS
                    bool invalid = false;
                    m_first->walk(EarlyExit{}, [&invalid](auto& buffer){
                        if (addr >= buffer.begin() && addr < buffer.end()) {
                            return true;
                        }
                        invalid = true;
                        return false;
                    });
                    if EXPECT_NOT_TAKEN(invalid) {
                        throw std::runtime_error("homogeneous::block_pools::FreeList discarded object not belonging to pool");
                    }
                    #endif
                    Item* item = reinterpret_cast<Item*>(object);
                    item->next = m_next;
                    m_next = item;
                }
                
                void reset () {
                    m_buffers.deallocate(m_first->next());
                    m_first->reset();
                    init_buffer(m_first);
                    m_next = first(m_first);
                    m_current = m_first;
                }

            private:
                Buffer* m_first;
                Buffer* m_current;
                BufferPool& m_buffers;
                ConcurrencyPolicyType m_concurrency_policy;

                union Item {
                    T object;
                    Item* next;
                };
                Item* m_next;

                Item* first (Buffer* buffer) const {
                    return reinterpret_cast<Item*>(buffer->begin());
                }

                void init_buffer (Buffer* buffer) const {
                    std::byte* ptr = buffer->begin();
                    do {
                        std::byte* next = ptr + block_size;
                        reinterpret_cast<Item*>(ptr)->next = reinterpret_cast<Item*>(next);
                        ptr = next;
                    } while (ptr < buffer->end());
                    reinterpret_cast<Item*>(ptr - block_size)->next = nullptr;
                }

                [[nodiscard]] T* allocate () {
                    Item* item;
                    if EXPECT_TAKEN(m_next != nullptr) {
                        // Get the item to return
                        item = m_next;
                    } else {
                        // Allocate a new buffer
                        auto* buffer = m_buffers.template allocate<OutOfSpacePolicyType>();
                        #ifndef MONKEYMEM_OMIT_OUTOFMEMORY_CHECKS
                        if EXPECT_NOT_TAKEN(buffer == nullptr) {
                            return nullptr;
                        }
                        #endif
                        // Link new buffer into chain
                        m_current->next(buffer);
                        // Set new buffer as current
                        m_current = buffer;
                        // Initialise free list
                        init_buffer(buffer);
                        // Get the item to return
                        item = first(buffer);
                    }
                    // Set the next free item
                    m_next = item->next;
                    // Return item
                    return &item->object;
                }
            };

            template <typename T, typename BufferPool, typename Policies=Policies<>>
            class Moving {
            public:
                using value_type = T;
                using ConcurrencyPolicyType = typename Policies::Concurrency::BlockAllocatorPolicy;
                using ItemAlignPolicyType = typename Policies::ItemAlign;
                using OutOfSpacePolicyType = typename Policies::OutOfSpace;
                using iterable = data_access::PagedIterable<T>;
                using const_iterable = data_access::PagedIterable<const T>;

                static constexpr std::size_t block_size = ItemAlignPolicyType::fit_multiple(sizeof(T));

                // Ctor taking unused T to allow template type inference
                Moving (T, BufferPool& buffers, std::size_t size) : Moving(buffers, size) {}
                // Normal ctors
                Moving (BufferPool& buffers, std::size_t size) :
                    m_first(buffers.allocate_static(size)),
                    m_current(m_first),
                    m_buffers(buffers)
                {}
                Moving (Moving&& other) :
                    m_first(other.m_first),
                    m_current(other.m_current),
                    m_buffers(other.m_buffers)
                {
                    other.m_first = nullptr;
                    other.m_current = nullptr;
                }
                ~Moving() {}

                template <typename... Args>
                T* emplace (Args&&... args) {
                }

                T* push_back (const T& item) {
                }
                
                T* push_back (T&& item) {
                }

                void discard (T* object) const {
                }
                
                void reset () {
                    if constexpr (! std::is_trivial<T>::value) {
                        // Not a trivial type, so need to call the constructor
                        each([](auto& it){
                            it.~T();
                        });
                    }
                }

                iterable iter () const {
                    // return iterable(m_impl.raw());
                }
                const_iterable const_iter () const {
                    // return iterable(m_impl.raw());
                }

                template<typename Func> void each (Func func) const {
                    auto it = iter();
                    do {
                        // Enforce constness of underlying if pool is const
                        if constexpr (std::is_const_v<decltype(this)>) {
                            for (const auto& item : it) {
                                func(item);
                            }
                        } else {
                            for (auto& item : it) {
                                func(item);
                            }
                        }
                    } while (it.next());
                }

            private:
                Buffer* m_first;
                Buffer* m_current;
                BufferPool& m_buffers;
                ConcurrencyPolicyType m_concurrency_policy;
            };
        }
    }

}

#ifdef MONKEYMEM_IMPL

#endif

#endif // _MONKEY_MEMORY__H_