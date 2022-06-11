#ifndef _MONKEY_MEMORY__H_
#define _MONKEY_MEMORY__H_

#include <cstdint>
#include <cstddef>
#include <functional>
#include <new>
#include <stdexcept>
#include <atomic>
#include <thread>

namespace monkeymem {

    #ifndef ERROR_LOG_FUNCTION
        namespace log {
            template <typename... Args> void error (Args... args) {}
        }
        #define ERROR_LOG_FUNCTION(fmt, ...) log::error
    #endif

    namespace helpers {
        template <typename T>
        T* align(T* pointer, const uintptr_t bytes_alignment) {
            intptr_t value = reinterpret_cast<intptr_t>(pointer);
            value += (-value) & (bytes_alignment - 1);
            return reinterpret_cast<T*>(value);
        }
    }

    namespace units {
        template <typename T> constexpr T kilobytes (T count) { return 1024 * count; }
        template <typename T> constexpr T megabytes (T count) { return 1024 * count; }
        template <typename T> constexpr T cachelines (T count) { return std::hardware_destructive_interference_size * count; }
    }

    namespace alignment {
        struct NoAlign {
            static constexpr int Boundary = 0;
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

        using AlignCacheLine = Aligned<std::hardware_destructive_interference_size>;
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
                ERROR_LOG_FUNCTION("Allocated more items than reserved space: ", name);
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

        std::byte* allocate (std::size_t bytes, std::size_t alignment=0) final {
            return m_allocate(bytes + alignment);
        }

        void release (std::byte* ptr, std::size_t size) final {
            m_deallocate(ptr, size);
        }

    private:
        Alloc m_allocate;
        Dealloc m_deallocate;
    };

    class Buffer {
    public:
        static Buffer create (MemoryAllocator& allocator, std::size_t size, std::uint32_t alignment=0) {
            std::byte* memory = allocator.allocate(size, alignment);
            std::byte* ptr = helpers::align(memory, alignment);
            std::uint32_t offset = ptr - memory;
            return {size - offset, offset, ptr};
        }

        static void destroy (MemoryAllocator& allocator, Buffer& buffer) {
            if (buffer.m_memory) {
                allocator.release(buffer.m_memory, buffer.m_size);
            }
            buffer.m_size = 0;
            buffer.m_memory = nullptr;
        }

        Buffer() : m_size(0), m_memory(nullptr) {}
        Buffer (std::size_t size, std::uint32_t offset, std::byte* memory) : m_size(size), m_offset(offset), m_memory(memory), m_next(nullptr) {}
        Buffer (Buffer&& other) : m_size(other.m_size), m_offset(other.m_offset), m_memory(other.m_memory), m_next(other.m_next) {
            other.m_size = 0;
            other.m_memory = nullptr;
        }
        ~Buffer () {}

        void operator= (Buffer&& other) {
            m_size = other.m_size;
            m_offset = other.m_offset;
            m_memory = other.m_memory;
            m_next = other.m_next;
            other.m_size = 0;
            other.m_memory = nullptr;
        }

        bool valid () const {
            return m_memory != nullptr;
        }

        std::size_t size () const {
            return m_size;
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

        // Set the end of the buffer
        void end (std::size_t e) {
            if (e > m_size) {
                throw std::out_of_range("Buffer end cannot be greater than size");
            }
            m_end = e;
        }

        std::byte& operator[](std::size_t index)
        {
            if (index < m_size) {
                return m_memory[index];
            } else {
                throw std::out_of_range("Buffer index out of range");
            }
        }

        Buffer view (std::size_t start_index, std::size_t size)
        {
            if (start_index + size <= m_size) {
                return Buffer{start_index + size, 0, m_memory + start_index};
            } else {
                return Buffer{};
            }
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
            m_end = 0;
        }

    private:
        std::size_t m_size;
        std::uint16_t m_offset;
        std::uint16_t m_end;
        std::byte*  m_memory;
        Buffer* m_next; // Buffers support linking
    };

    template <typename Alignment=alignment::NoAlign>
    class BufferPool {
    public:
        static constexpr int AlignmentBoundary = Alignment::Boundary;
        using iterator = std::vector<Buffer>::iterator;
        using const_iterator = std::vector<Buffer>::const_iterator;

        BufferPool (MemoryAllocator& allocator, std::size_t count, std::size_t buffer_size) : m_allocator(allocator), m_next(0) {
            for (auto i=0; i<count; ++i) {
                m_buffer.emplace_back(Buffer::create(allocator, buffer_size, AlignmentBoundary));
            }
        }
        ~BufferPool () {}

        const Buffer& operator[] (std::size_t index) const {
            return m_buffer[index];
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
                Buffer::release(buffer);
            }
            m_buffers.clear();
        }

        Buffer& allocate () {
            return m_buffers[m_next.fetch_add(1)];
        }

        void reset () {
            auto last = m_next.load();
            for (auto i=0; i<last; ++i) {
                m_buffers[i].reset();
            }
            m_next.store(0);
        }

        Buffer& allocate_static (std::size_t buffer_size) {
            m_static_buffers.emplace_back(Buffers::create(m_allocator, buffer_size, AlignmentBoundary));
            return m_static_buffers.back();
        }

    private:
        std::vector<Buffer> m_static_buffers;
        std::vector<Buffer> m_buffers;
        MemoryAllocator& m_allocator;
        std::atomic_uint32_t m_next;
    };

    namespace heterogeneous {

        namespace impl {
            template <typename Impl, typename ItemAlign = alignment::NoAlign, typename OutOfSpacePolicy = out_of_space_policies::Throw>
            class BaseStackPool {
            private:
                template <typename T>
                T* alloc () {
                    return ItemAlign::template align<T>(unaligned_allocate(ItemAlign::adjust_size(sizeof(T))));
                }
            public:
                using ItemAlignType = ItemAlign;
                using OutOfSpacePolicyType = OutOfSpacePolicy;

                BaseStackPool (BufferPool& buffers, std::size_t size) :
                    m_first(&buffers.allocate_static(size)),
                    m_current(m_first),
                    m_buffers(buffers),
                {}
                BaseStackPool (BaseStackPool&& other) :
                    m_first(other.m_first),
                    m_current(other.m_current),
                    m_buffers(other.m_buffers)
                {
                    other.m_first = nullptr;
                    other.m_current = nullptr;
                }
                virtual ~BaseStackPool() {}

                // Allocate, but don't construct
                std::byte* unaligned_allocate (std::uint32_t bytes) {
                    auto offset = Impl::fetch_add(bytes);
                    if (offset + bytes > m_current->size()) {
                        Impl::synced(
                            // Condition to decide whether to run the update, only needed in atomic version
                            [&offset, bytes](){
                                // Used to recheck, but also for the side effect of setting offset
                                offset = Impl::fetch_add(bytes);
                                // Check whether the buffers still need updating
                                return offset + bytes > m_current->size();
                            },
                            // Update the buffers
                            [this, &offset, bytes](){
                                // The first thread to reach the synced block must allocate a new buffer
                                auto* buffer = &m_buffers.allocate();
                                // Set the end of the buffer
                                m_current->end(offset);
                                // Link new buffer into chain
                                m_current->next(buffer);
                                // Set new buffer as current
                                m_current = buffer;
                                // Allocate bytes in new buffer
                                Impl::put(bytes);
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
                T* emplace (Args&&... args) {
                    return new(alloc<T>()) T{args...};
                }

                template <typename T>
                void push_back (const T& item) {
                    return new(alloc<T>()) T{item};
                }

                void reset () {
                    Impl::put(0);
                    m_first->reset();
                    m_current = m_first;
                }

                // Access underlying data
                const Buffer& data () const {
                    // Make sure the latest buffer is properly sized
                    m_current->end(load());
                    // Return a reference to the first buffer
                    return *m_first;
                }

            private:
                Buffer* m_first;
                Buffer* m_current;
                BufferArray& m_buffers;
            };
        }

        // A basic stack allocator. Objects can be allocated from the top of the stack, but are deallocated all at once. Pointers to elements are stable until reset() is called.
        template <typename ItemAlign = alignment::NoAlign, typename OutOfSpacePolicy = out_of_space_policies::Throw>
        class StackPool : public impl::BaseStackPool<StackPool, ItemAlign, OutOfSpacePolicy> {
        public:
            StackPool (BufferPool& buffers, std::size_t size) : impl::BaseStackPool<StackPool, ItemAlign, OutOfSpacePolicy>(buffers, size), next(0) {}
            StackPool (StackPool&& other) : impl::BaseStackPool<StackPool, ItemAlign, OutOfSpacePolicy>(std::move(other)), next(other.next) {}
            virtual ~StackPool() {}
        private:
            std::uint32_t next;
            std::uint32_t fetch () const { return next; }
            std::uint32_t fetch_add (std::uint32_t amount) {
                auto cur = next;
                next += amount;
                return cur;
            }
            void put (std::uint32_t value) { next = value; }
            template <typename Condition, typename Callback> void synced (Condition, Callback callback) { callback(); } // Condition optimized out in non-atomic version
        };

        // Same as StackPool, but uses an atomic next pointer allowing multiple threads to allocate objects from it concurrently. Pointers to elements are stable until reset() is called.
        template <typename ItemAlign = alignment::NoAlign, typename OutOfSpacePolicy = out_of_space_policies::Throw>
        class AtomicStackPool : public impl::BaseStackPool<AtomicStackPool, ItemAlign, OutOfSpacePolicy> {
        public:
            AtomicStackPool (BufferPool& buffers, std::size_t size) : impl::BaseStackPool<AtomicStackPool, ItemAlign, OutOfSpacePolicy>(buffers, size), next(0) {}
            AtomicStackPool (AtomicStackPool&& other) : impl::BaseStackPool<AtomicStackPool, ItemAlign, OutOfSpacePolicy>(std::move(other)), next(other.next) {}
            virtual ~AtomicStackPool() {}
        private:
            std::atomic_uint32_t next;
            std::uint32_t fetch () const final { return next.load(); }
            std::uint32_t fetch_add (std::uint32_t amount) final { return next.fetch_add(amount); }
            void put (std::uint32_t value) final { next.store(value); }

            std::mutex m_mutex;
            template <typename Condition, typename Callback> void synced (Condition condition, Callback callback) {
                std::unique_lock<std::mutex> lock(m_mutex);
                if (condition()) {
                    callback();
                }
            }
        };
    }

}

#ifdef MONKEYMEM_IMPL



#endif

#endif // _MONKEY_MEMORY__H_