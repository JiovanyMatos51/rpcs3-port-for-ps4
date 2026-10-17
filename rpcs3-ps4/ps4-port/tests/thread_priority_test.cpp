#include "util/ps4_thread_priority.hpp"
#include <cassert>
#include <iostream>

int main()
{
    utils::ps4::thread_priority priority;
    int running = 700, reads = 0, writes = 0, get_error = 0, set_error = 0;
    const auto get = [&](int* result)
    {
        ++reads;
        if (get_error) return get_error;
        *result = running;
        return 0;
    };
    const auto set = [&](int value)
    {
        ++writes;
        if (set_error) return set_error;
        running = value;
        return 0;
    };
    // Native scheduler chooses lower numeric priorities first. A raised RSX
    // must precede a guest at 700 and restoration must make them peers again.
    assert(priority.apply(0, get, set) == 0 && reads == 0 && writes == 0);
    assert(priority.apply(1, get, set) == 0 && running < 700 && running > 256);
    assert(priority.apply(1, get, set) == 0 && reads == 1);
    assert(priority.apply(0, get, set) == 0 && running == 700);
    assert(priority.apply(-1, get, set) == 0 && running > 700 && running < 767);
    assert(priority.apply(0, get, set) == 0 && running == 700);
    running = 259;
    assert(priority.apply(1, get, set) == 0 && running == 256);
    assert(priority.apply(0, get, set) == 0 && running == 259);
    running = 764;
    assert(priority.apply(-1, get, set) == 0 && running == 767);
    assert(priority.apply(0, get, set) == 0 && running == 764);
    running = 700;
    get_error = 3;
    const int before = writes;
    assert(priority.apply(1, get, set) == 3 && writes == before && running == 700);
    get_error = 0;
    set_error = 4;
    assert(priority.apply(1, get, set) == 4 && running == 700);
    set_error = 0;
    assert(priority.apply(1, get, set) == 0 && running < 700);
    set_error = 5;
    assert(priority.apply(0, get, set) == 5 && running < 700);
    set_error = 0;
    assert(priority.apply(0, get, set) == 0 && running == 700);
    std::cout << "PASS: native priority direction, bounds, restoration and API failures\n";
}
