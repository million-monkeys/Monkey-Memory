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

namespace monkeymem {

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
            static std::uint32_t adjust_size (std::uint32_t size) { return size; }
            template <typename T> static T* align (void* buffer) {
                return reinterpret_cast<T*>(buffer);
            }
        };

        template <int BoundaryT>
        struct Aligned {
            static constexpr int Boundary = BoundaryT;
            static std::uint32_t adjust_size (std::uint32_t size) { return size + BoundaryT; }
            template <typename T> static T* align (void* buffer) {
                return reinterpret_cast<T*>(helpers::align(buffer, BoundaryT));
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
        public:
            Unsafe () : next(0) {}
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
            template <typename Condition, typename Callback>
            void synced (Condition, Callback callback) const {
                callback(); // Condition optimized out in non-atomic version
            }
        private:
            std::uint32_t next;
        };

        struct Atomic {
        public:
            Atomic () : next(0) {}
            std::uint32_t fetch () const {
                return next.load();
            }
            std::uint32_t fetch_add (std::uint32_t amount) {
                return next.fetch_add(amount);
            }
            void put (std::uint32_t value) {
                next.store(value);
            }
            template <typename Condition, typename Callback>
            void synced (Condition condition, Callback callback) const {
                std::unique_lock<std::mutex> lock(m_mutex);
                if (condition()) {
                    callback();
                }
            }
        private:
            std::atomic_uint32_t next;
            mutable std::mutex m_mutex;
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

        std::byte* data () {
            return m_memory;
        }

        std::byte* begin () const {
            return m_memory;
        }

        std::byte* end () const {
            return m_memory + m_end;
        }

        std::byte& operator[] (std::size_t index) {
            if (index < m_end) {
                return m_memory[index];
            } else {
                throw std::out_of_range("Buffer index out of range");
            }
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
            if (e > m_size) {
                throw std::out_of_range("Buffer end cannot be greater than size");
            }
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

    private:
        std::byte*  m_memory;
        Buffer* m_next; // Buffers support linking
        std::uint16_t m_size;
        std::uint16_t m_end;

    public:
        // Walk through this and each linked buffer
        template <typename Func> static void walk (const Buffer& first, Func func) {
            const Buffer* cur = &first;
            do {
                func(*cur);
                cur = cur->next();
            } while (cur != nullptr);
        }
    };

    template <typename Alignment=alignment::NoAlign>
    class BufferPool {
    public:
        static constexpr int AlignmentBoundary = Alignment::Boundary;
        using iterator = std::vector<Buffer>::iterator;
        using const_iterator = std::vector<Buffer>::const_iterator;

        BufferPool (MemoryAllocator& allocator, std::size_t count, std::size_t buffer_size) : m_allocator(allocator), m_next(0) {
            for (auto i=0; i<count; ++i) {
                m_buffers.emplace_back(Buffer::create(allocator, buffer_size, AlignmentBoundary));
            }
        }
        BufferPool (BufferPool&& other) : m_static_buffers(std::move(other.m_static_buffers)), m_buffers(std::move(other.m_buffers)), m_allocator(other.m_allocator), m_next(other.m_next) {
            other.m_next = 0;
        }
        ~BufferPool () {
            clear();
        }

        const Buffer& operator[] (std::size_t index) const {
            return m_buffers[index];
        }

        iterator begin () {
            return m_buffers.begin();
        }

        const_iterator begin () const {
            return m_buffers.begin();
        }

        iterator end () {
            return m_buffers.end();
        }

        const_iterator end () const {
            return m_buffers.end();
        }

        std::size_t size () const {
            return m_buffers.size();
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

        template <typename OutOfSpacePolicy=out_of_space_policies::Throw>
        Buffer* allocate () {
            auto index = m_next.fetch_add(1);
            if (index < m_buffers.size()) {
                return &m_buffers[index];
            } else {
                return OutOfSpacePolicy::template apply<Buffer>("BufferPool");
            }
        }

        void reset () {
            auto last = m_next.load();
            for (auto i=0; i<last; ++i) {
                m_buffers[i].reset();
            }
            m_next.store(0);
        }

        Buffer* allocate_static (std::size_t buffer_size) {
            m_static_buffers.emplace_back(Buffer::create(m_allocator, buffer_size, AlignmentBoundary));
            return &m_static_buffers.back();
        }

    private:
        std::vector<Buffer> m_static_buffers;
        std::vector<Buffer> m_buffers;
        MemoryAllocator& m_allocator;
        std::atomic_uint32_t m_next;
    };

    namespace heterogeneous {

        // A basic stack allocator. Objects can be allocated from the top of the stack, but are deallocated all at once. Pointers to elements are stable until reset() is called.
        template <typename ConcurrencyPolicy = concurrency_policies::Unsafe, typename ItemAlign = alignment::NoAlign, typename OutOfSpacePolicy = out_of_space_policies::Throw>
        class StackPool {
        private:
            template <typename T>
            T* alloc () {
                return ItemAlign::template align<T>(unaligned_allocate(ItemAlign::adjust_size(sizeof(T))));
            }
        public:
            using ItemAlignType = ItemAlign;
            using OutOfSpacePolicyType = OutOfSpacePolicy;

            template <typename BufferPool>
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
            virtual ~StackPool() {}

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
                        // Update the buffers
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
                }
                return m_current->data() + offset;
            }

            std::byte* allocate (std::uint32_t bytes) {
                return ItemAlign::template align<std::byte>(unaligned_allocate(ItemAlign::adjust_size(bytes)));
            }
                
            // Allocate and construct
            template <typename T, typename... Args>
            T& emplace (Args&&... args) {
                return *new(alloc<T>()) T{args...};
            }

            template <typename T>
            void push_back (const T& item) {
                new(alloc<T>()) T{item};
            }

            void reset () {
                ConcurrencyPolicy::put(0);
                m_first->reset();
                m_current = m_first;
            }

            // Access underlying data
            Buffer data () const {
                // The current size of the latest buffer must be recorded
                m_concurrency_policy.synced(
                    [](){ return true; },
                    [this](){
                        m_current->end(m_concurrency_policy.fetch());
                    }
                );
                // Then the first buffer is returned
                return m_first->view(true);
            }

        private:
            Buffer* m_first;
            Buffer* m_current;
            BufferPool<ItemAlignType>& m_buffers;
            ConcurrencyPolicy m_concurrency_policy;
        };

    }

}

#ifdef MONKEYMEM_IMPL



#endif

#endif // _MONKEY_MEMORY__H_