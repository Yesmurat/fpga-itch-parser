#include "price_book.hpp"
#include <cassert>
#include <cstdio>

void test_increment_new_level() {

    book::PriceLevel level0 {250, 123450};
    book::PriceBook book0{true};

    auto result = book0.increment(level0.price, level0.aggregate_shares);
    assert(result == book::PriceBook::UpdateResult::Ok);
    assert(book0.count() == 1);
    assert(book0.data()[0].price == level0.price);
    assert(book0.data()[0].aggregate_shares == level0.aggregate_shares);

}

void test_increment_existing_level() {

    book::PriceLevel level0 {250, 123450};
    book::PriceBook book0{true};

    book0.increment(level0.price, level0.aggregate_shares);
    auto result = book0.increment(level0.price, level0.aggregate_shares);

    assert(result == book::PriceBook::UpdateResult::Ok);
    assert(book0.count() == 1);
    assert(book0.data()[0].price == level0.price);
    assert(book0.data()[0].aggregate_shares == (level0.aggregate_shares * 2));

}

int main(int argc, char** argv) {

    test_increment_new_level();
    test_increment_existing_level();

    puts("All Price Book tests passed.\n");
    return 0;
}