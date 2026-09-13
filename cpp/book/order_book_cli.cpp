#include <fstream>
#include <iostream>
#include <vector>
#include "order_book.hpp"
#include "itch_decoder.hpp"

namespace {

    void print(const book::BookUpdate& book_update) {

        std::string buy_or_ask = book_update.is_buy ? "Buy" : "Ask";

        std::cout << "Symbol index of a newly applied message: " << (int) book_update.symbol_index << "\n";
        std::cout << "Is it a Buy or Ask:                      " << buy_or_ask               << "\n";
        std::cout << "The best price in the book:              " << book_update.best_price   << "\n";
        std::cout << "The biggest shares in the book:          " << book_update.best_shares  << "\n";
        std::cout << "Sequence number:                         " << book_update.seq_num      << "\n";

    }

}

int main(int argc, char* argv[]) {

    std::vector<itch::DecodedMessage> messages;

    if (argc > 1) {

        std::ifstream f(argv[1], std::ios::binary);

        if (!f) {
            std::cerr << "order_book_cli: cannot open " << argv[1] << "\n";
            return 1;
        }

        messages = itch::decode_all(f);

    }

    else { // if no file argument is given.

        messages = itch::decode_all(std::cin);

    }

    book::OrderBook order_book{};

    auto callback = [&](const book::BookUpdate& incoming_book_update) {
        print(incoming_book_update);
    };

    order_book.set_callback(callback);

    for (size_t i = 0; i < messages.size(); i++) {
        order_book.apply(messages[i]);
    } // apply each message to the order book

    // print statistics.
    std::cout << "invalid_price_decrement:   " << order_book.report_stats().invalid_price_decrement   << "\n";
    std::cout << "invalid_reduction:         " << order_book.report_stats().invalid_reduction         << "\n";
    std::cout << "locate_capacity_exceeded:  " << order_book.report_stats().locate_capacity_exceeded  << "\n";
    std::cout << "order_table_insert_failed: " << order_book.report_stats().order_table_insert_failed << "\n";
    std::cout << "unknown_order_ref:         " << order_book.report_stats().unknown_order_ref         << "\n";

    return 0;

}