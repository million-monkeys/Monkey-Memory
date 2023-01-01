
#include <iostream>
template <typename... Args>
void log (Args... args) {
    ((std::cout << std::forward<Args>(args)), ...);
    std::cout << "\n";
    std::cout.flush();
}
#define ERROR_LOG_FUNCTION log

#include <monkeymem.hpp>

namespace mm = monkeymem;

struct A {
    int a;
};
struct B {
    int a, b;
};
struct C {
    char c[17];
};

int main (int argc, char** argv)
{
    mm::MemoryAllocatorWrapper allocator{
        [](auto size){
            return new std::byte[size];
        },
        [](auto* ptr, auto size){
            delete [] ptr;
        },
    };
    mm::Buffer buffer = mm::Buffer::create(allocator, 120, 64);
    mm::Buffer::destroy(allocator, buffer);

    mm::buffer_pools::FreeList buffers{allocator, 100, mm::units::kilobytes(5)};

    // mm::heterogeneous::StackPool pool(buffers, mm::units::kilobytes(10));

    mm::heterogeneous::StackPool<mm::buffer_pools::FreeList<>, mm::Policies<mm::concurrency_policies::Unsafe>> pool(buffers, mm::units::kilobytes(10));

    pool.push_back(A{1});

    int items =  (mm::units::kilobytes(10) / sizeof(B)) * 3 - 1;
    log("Number of items: ", items, " ", mm::units::kilobytes(10), " ", sizeof(B));
    for (auto i = 0; i < items; i++) {
        pool.push_back(B{2, 3});
    }

    pool.data().walk([](auto& buffer){
        std::cout << buffer.size() << "\n";
        return false;
    });

    buffers.reset();

    mm::homogeneous::StackPool pool_a(A{}, buffers, mm::units::kilobytes(10));
    int items2 =  (mm::units::kilobytes(10) / sizeof(A)) * 3 - 1;
    log("Number of items: ", items2, " ", mm::units::kilobytes(10), " ", sizeof(A));
    unsigned expected = 0;
    for (auto i = 0; i < items; i++) {
        pool_a.emplace(i);
        expected += i;
    }

    // Iterate through pool
    unsigned result = 0;
    pool_a.each([&result](const auto& obj){
        result += obj.a;
    });
    log("Expected = ", expected, " Result = ", result, " ", expected == result ? "SUCCESS" : "FAILED");

    mm::homogeneous::block_pools::FreeList<B, decltype(buffers), mm::Policies<mm::default_policies::Concurrency, mm::alignment::AlignSIMD>> block_pool(buffers, mm::units::kilobytes(2));

    log("Adding to block pool");
    [[maybe_unused]] B* item = block_pool.emplace(1, 2);

    return 0;
}
