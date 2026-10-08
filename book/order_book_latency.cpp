#include "order_book.hpp"
#include "itch_decoder.hpp"
#include <fstream>
#include <iostream>
#include <chrono>
#include <vector>
#include <algorithm>

std::chrono::nanoseconds percentile_value (const std::vector<std::chrono::nanoseconds>& nanosecond_durations, double percentile) { // the helper assumes the vector is sorted, non-empty, and that percentile is between 0 and 100.

    size_t n                 = nanosecond_durations.size();
    size_t percentile_index  = (percentile / (double)100) * (n - 1);

    return nanosecond_durations[percentile_index];

}

int main(int argc, char* argv[]) {

    std::vector<itch::DecodedMessage> messages;

    if (argc > 1) {

        std::ifstream f(argv[1], std::ios::binary);
        
        if (!f) {
            std::cerr << "order_book_latency: cannot open " << argv[1] << "\n";
            return 1;
        }

        messages = itch::decode_all(f);

    }

    else {
        messages = itch::decode_all(std::cin);
    }

    book::OrderBook order_book{};

    std::chrono::steady_clock clock;
    std::chrono::_V2::steady_clock::time_point start_time;

    std::vector<std::chrono::nanoseconds> nanosecond_durations;

    auto callback = [&](const book::BookUpdate&) {

        auto end_time = clock.now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time);
        nanosecond_durations.push_back(duration);

    };

    order_book.set_callback(callback);

    // apply each message to the order book.
    for (size_t i = 0; i < messages.size(); i++) {
        start_time = clock.now();
        order_book.apply(messages[i]);
    }

    // sort nanosecond_durations
    std::sort(nanosecond_durations.begin(), nanosecond_durations.end());

    if (nanosecond_durations.size() != 0) {
        std::cout << "p50:          " << percentile_value(nanosecond_durations, 50).count()            << "ns\n";
        std::cout << "p90:          " << percentile_value(nanosecond_durations, 90).count()            << "ns\n";
        std::cout << "p99:          " << percentile_value(nanosecond_durations, 99).count()            << "ns\n";
        std::cout << "min:          " << nanosecond_durations[0].count()                               << "ns\n";
        std::cout << "max:          " << nanosecond_durations[nanosecond_durations.size() - 1].count() << "ns\n";
        std::cout << "sample count: " << nanosecond_durations.size()                                   << "\n"  ;
    }

    else {
        std::cout << "no samples are present.\n";
    }
    
    return 0;

}