# Monkey-Memory
Memory management library

Library for memory pooling and allocation based on the idea of a linked list of buffers.

Everything is in the `monkeymem` namespace. This namespace is omitted in the example code below.

# Raw memory allocation

The library does not allocate its own memory, instaed the user must provide an allocator by subclassing the `MemoryAllocator` class:

```cpp
class MemoryAllocator {
public:
    virtual std::byte* allocate (std::size_t bytes, std::size_t alignment) = 0;
    virtual void release (std::byte* ptr, std::size_t size) = 0;
};
```

A helper class `MemoryAllocatorWrapper` is provided that wraps an allocation and deallocation function:

```cpp
std::byte* alloc (std::size_t size) { return new std::byte[size]; }
void dealloc (std::byte* ptr, std::size_t size) { delete [] ptr; }

MemoryAllocatorWrapper allocator{alloc, dealloc};
// Or using lambdas:
MemoryAllocatorWrapper allocator{
    [](auto size){ return new std::byte[size]; },
    [](auto* ptr, auto){ delete [] ptr; },
};
```

# Buffers

A buffer can be allocated and deallocated using an allocator:
```cpp
std::size_t buffer_size_in_bytes = 120;
Buffer buffer = mm::Buffer::create(allocator, buffer_size_in_bytes);
Buffer::destroy(allocator, buffer);
```
Buffers support being moved:
```cpp
Buffer a = ...;
Buffer b = std::move(a);
Buffer c{std::move(b)};
a.valid(); // returns false
b.valid(); // returns false
c.valid(); // returns true
```
The buffers underlying data can be accessed with `.data()` and `.size()`:
```cpp
std::size_t buffer_size_in_bytes = 120;
Buffer buffer = mm::Buffer::create(allocator, buffer_size_in_bytes);
std::byte* raw_data = buffer.data();
std::size_t data_size = buffer.size();
assert(data_size == buffer_size_in_bytes);
```
A buffers contents can be indexed into by byte index. Accessing an out of range index will throw an `std::out_of_range` exception.
```cpp
buffer[10] = 0x1f;
```
A view into a buffer can be created. This will return a new buffer that points into the content of the original buffer:
```cpp
std::size_t buffer_size_in_bytes = 120;
Buffer buffer = mm::Buffer::create(allocator, buffer_size_in_bytes);
Buffer view = buffer.view(10, 20); // 10 = start index, 20 = size in bytes
assert(view[0] == buffer[10]);
assert(view.data() + view.size() == buffer.data() + 10 + 20);
```
Buffers can also be chained together in a linked list:
```cpp
Buffer buffer1 = mm::Buffer::create(allocator, buffer_size_in_bytes);
Buffer buffer2 = mm::Buffer::create(allocator, buffer_size_in_bytes);
buffer1.next(&buffer2);
assert(buffer1.next() == &buffer2);
```
Monkey Memory also provides iterators that seamlessly iterate through objects allocated into a chain of buffers, allowing them to be accessed as if they were a single large buffer. 

# BufferPool

Normally, buffers are not created individually as in the above examples. Typically, a collection of buffers would be allocated using a `BufferPool`.

A `BufferPool` contains two collections of buffers:
* Static buffers, these buffers are allocated once and kept for the lifetime of the `BufferPool`.
* Dynamic buffers, these are buffers that are allocated on demand and released all at once at a sync point (eg once per game frame)

Ironically, static buffers are allocated dynamically on demand while dynamic buffers are allocated once statically on creation. Allocating a static buffer is an expensive operation, as memory must be allocated for it, but allocating a dynamic buffer from the pool is cheap (incrementing an atomic integer).

Creating a `BufferPool`:
```cpp
BufferPool buffers{
  allocator, // The allocator, as above
  100,       // Number of dynamic buffers contained in the pool
  1024       // Size, in bytes, of each dynamic buffer
};
```

Buffers can be allocated:
```cpp
Buffer* buffer1 = buffers.allocate_static(512); // Allocate static buffer of 512 bytes in size
Buffer* buffer2 = buffers.allocate(); // Allocate one of the 100 pre-allocated dynamic buffers

buffers.reset(); // buffer2 is now invalid and must no longer be used. buffer1 is still valid
```

Typically, static buffers are used as the *first* buffer in a chain, while dynamic buffers are the subsequent buffers that are added on demaind from the pre-allocated pool. It is recommended that the static buffer is sized to accommodate the average size requirement, so that the typical use of the buffer will fit within a single static buffer, and that dynamic buffers are used to handle the cases when there is a spike in requirements. For example, a typical use case would be a game event system: if most frames are expected to dispatch 10 events, then the first (static) buffer in the chain should be sized to fit 10 events (or perhaps slightly more like 11 or 12), but if there is a sudden spike in activity that requires 15 or 20 events, dynamic buffers are added to the chain for that frame to accommodate the extra events.

## Out of Space Policies

If an attempt is made to allocate a dynamic buffer when there are no more unused buffers remaining in the pool, by default, an `std::runtime_error` exception is thrown. However, this can be controlled using an `out of space policy`. Three policies exist, but it is possible to create custom ones if the need arises:

### `out_of_space_policies::Throw`
This is the default. If the `BufferPool` does not have any unused buffers to allocate, an exception is thrown.
```cpp
Buffer* buffer1 = buffers.allocate();
Buffer* buffer2 = buffers.allocate<out_of_space_policies::Throw>(); // Same as above
```
### `out_of_space_policies::Log`
This policy will log the error and then continue by returning a `nullptr` instead of a pointer to a buffer.
```cpp
Buffer* buffer1 = buffers.allocate<out_of_space_policies::Log>();
```
A log function must be provided by defining the `ERROR_LOG_FUNCTION` macro. This function should take at least two arguments:
```
void log_func (const std::string& label, const std::string& name)
```
To use it with spdlog:
```
#define ERROR_LOG_FUNCTION(a, b) spdlog::error("{}: {}", a, b)
```
### `out_of_space_policies::Ignore`
This policy silently ignores the error and returns `nullptr`.
```cpp
Buffer* buffer1 = buffers.allocate<out_of_space_policies::Ignore>();
```

# Pools

Similar to how Buffers are not expected to be created directly, it is also not expected that `BufferPools` are used to allocate buffers directly. Instead, they should be created (their dynamic pool preallocated) and then passed to higher level abstractions to actually manage buffer allocations. One such abstraction is the `Pool`, which is a buffer-backed utility to allocate chunks of memory. Pools can be typed (they allocate typed objects) or untyped (they allocate a series of bytes) and they can be heterogeneous (each allocation can be of a different size) or homogeneous (each allocation is identical). Typed pools are built on top of untyped pools.

# Heterogeneous Pools

Heterogeneous pools dish out varying amounts of bytes from an underlying buffer, handle buffer chaining internally and come in both atomic and non-atomic forms. The non-atomic pools are slightly more efficent, but cannot be safely used across multiple threads unsynchronized.

## StackPool

The `StackPool` allocates bytes as if from a stack: each subsequent allocation uses the next bytes after the previous allocation. There is no way to deallocate any particular allocation, but the entire pool can be `reset` all at once, deallocating everything all at once (and releasing any chained dynamic buffers back to the underlying `BufferPool`).
As the heterogeneous `StackPool` only operates on bytes, no destructors will be called on any allocated objects on reset, so the allocated memory should only be used to store POD types.

`StackPool`'s can be created from a `BufferPool`:
```cpp
BufferPool buffers{allocator, 100, 1024};

std::size_t size_of_static_buffer = 100; // In bytes
StackPool pool{buffers, size_of_static_buffer};
```
Once a pool has been created, chunks of memory may be allocated from it:
```cpp
std::byte* ptr1 = pool.allocate(32); // Allocate 32 bytes
std::byte* ptr2 = pool.allocate(7); // Allocate 7 bytes
```
Helpers are provided for allocating typed objects from a `StackPool`. These objects *must* be POD types:
```cpp
struct Foo { int a; };
Foo* foo1 = pool.emplace<Foo>(10);
Foo foo2{12};
pool.push_back(foo2);
```
The `StackPool` can be reset, making its alloocatings start from the start of the buffer again, effectively freeing the allocated memory:
```
pool.reset();
```
