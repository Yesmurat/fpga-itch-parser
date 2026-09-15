#include <fstream>
#include <iostream>
#include <chrono>
#include <vector>
#include "order_book.hpp"
#include "itch_decoder.hpp"

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
    book::BookUpdate current_book_update;

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

    for (const auto& duration : nanosecond_durations) {
        std::cout << duration.count() << "ns\n";
    }
    
    return 0;

}