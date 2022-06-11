#include <monkeymem.hpp>

namespace mm = monkeymem;

int main (int argc, char** argv)
{
    mm::MemoryAllocatorWrapper allocator{
        [](auto size){ return new std::byte[size]; },
        [](auto* ptr, auto){ delete [] ptr; },
    };
    mm::Buffer buffer = mm::Buffer::create(allocator, 120);
    mm::Buffer::destroy(allocator, buffer);

    mm::BufferPool buffers{allocator, 100, mm::units::kilobytes(5)};

    mm::heterogeneous::StackPool pool(buffers, mm::units::kilobytes(10));

    buffers.clear();

    return 0;
}
