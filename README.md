# Monkey-Memory
Memory management library

Library for memory pooling and allocation based on the idea of a linked list of buffers.

Everything is in the `monkeymem` namespace. This namespace is omitted in the example code below.

## Raw memory allocation

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

Ironically, static buffers are allocated dynamically on demand while dynamic buffers are allocated once statically on creation.

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
Buffer& buffer1 = buffers.allocate_static(512); // Allocate static buffer of 512 bytes in size
Buffer& buffer2 = buffers.allocate(); // Allocate one of the 100 pre-allocated dynamic buffers

buffers.reset(); // buffer2 is now invalid and must no longer be used. buffer1 is still valid
```

Typically, static buffers are used as the *first* buffer in a chain, while dynamic buffers are the subsequent buffers that are added on demaind from the pre-allocated pool. Allocating a static buffer is an expensive operation, as memory must be allocated for it, but allocating a dynamic buffer from the pool is cheap (incrementing an atomic integer).

