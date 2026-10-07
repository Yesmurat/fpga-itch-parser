#include <fstream>
#include <iostream>
#include <chrono>
#include <vector>
#include <algorithm>
#include "order_book.hpp"
#include "itch_decoder.hpp"

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
    // book::BookUpdate current_book_update;

    std::chrono::steady_clock clock; // clock variable
    std::chrono::_V2::steady_clock::time_point start_time;

    std::vector<std::chrono::nanoseconds> nanosecond_durations;

    auto callback = [&](const book::BookUpdate& book_update) {

        auto end_time = clock.now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time);
        nanosecond_durations.push_back(duration);

    };

    order_book.set_callback(callback);

    for (size_t i = 0; i < messages.size(); i++) {

        start_time = clock.now();
        order_book.apply(messages[i]);

    } // apply each message to the order book.

    // removed for generate_stream.py testing...
    // for (const auto& duration : nanosecond_durations) {
    //     std::cout << duration.count() << "ns\n";
    // }

    // sort nanosecond_durations
    std::sort(nanosecond_durations.begin(), nanosecond_durations.end());
    // std::sort() changes the vector, doesn't return newly created one.

    // pick values at p/100 * (n - 1) and print p50, p90, p99 (and p99.9 once n is large enough)
    // plus min, max, and sample count.
    // p is the percentile you want: 50 for p50(the median), 90 for p90, 99.9 for p99.9
    // n is the number of samples in the sorted vector, which is basically nanosecond_durations.size()

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