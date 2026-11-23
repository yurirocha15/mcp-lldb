// The program the tests debug. Comments of the form "MARK:<name>" label lines the tests set
// breakpoints on; the tests find them by reading this file.

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#include <sys/prctl.h>
#endif

struct Point {
    int x;
    int y;
};

int g_counter = 0;

int add(int a, int b) {
    int sum = a + b;  // MARK:add_body
    return sum;
}

void bump(int times) {
    for (int i = 0; i < times; ++i) {
        g_counter += 1;  // MARK:bump_body
    }
}

int read_x(const Point* point) {
    return point->x;  // MARK:crash
}

void spin() {
    volatile bool running = true;
    while (running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));  // MARK:spin_body
    }
}

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "normal";
    Point origin{3, 4};
    std::vector<int> values{1, 2, 3};
    std::printf("starting %s\n", mode.c_str());
    std::fflush(stdout);
    const int total = add(origin.x, origin.y);  // MARK:call_add
    bump(3);
    if (mode == "crash") {
        return read_x(nullptr);
    }
    if (mode == "spin") {
        spin();
    }
    if (mode == "wait") {
        // Let any process trace this one, so that a test can attach to it or dump its core.
#if defined(__linux__) && defined(PR_SET_PTRACER)
        prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
#endif
        spin();
    }
    if (mode == "threads") {
        std::thread worker(spin);
        worker.join();
    }
    std::printf("total %d counter %d size %zu\n", total, g_counter, values.size());  // MARK:report
    return total;
}
