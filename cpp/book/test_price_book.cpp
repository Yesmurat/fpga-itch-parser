#include "price_book.hpp"
#include <cassert>
#include <cstdio>

void test_increment_new_level() {

    book::PriceLevel level0 {250, 123450};
    book::PriceBook book_bid{true};
    book::PriceBook book_ask{false};

    // bid assertions.
    auto result_bid = book_bid.increment(level0.price, level0.aggregate_shares);
    assert(result_bid == book::PriceBook::UpdateResult::Ok);
    assert(book_bid.count() == 1);
    assert(book_bid.data()[0].price == level0.price);
    assert(book_bid.data()[0].aggregate_shares == level0.aggregate_shares);
    
    // ask assertions.
    auto result_ask = book_ask.increment(level0.price, level0.aggregate_shares);
    assert(result_ask == book::PriceBook::UpdateResult::Ok);
    assert(book_ask.count() == 1);
    assert(book_ask.data()[0].price == level0.price);
    assert(book_ask.data()[0].aggregate_shares == level0.aggregate_shares);

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

void test_increment_sorted_order() {

    book::PriceBook book_bid{true};
    book::PriceBook book_ask{false};

    book::PriceLevel level1{305000, 200};   // $30.50, 200 shares
    book::PriceLevel level2{125000, 450};   // $12.50, 450 shares
    book::PriceLevel level3{498000, 75};    // $49.80, 75 shares
    book::PriceLevel level4{210000, 1000};  // $21.00, 1000 shares
    book::PriceLevel level5{350000, 320};   // $35.00, 320 shares

    // bid.
    auto result1_bid = book_bid.increment(level1.price, level1.aggregate_shares);
    assert(result1_bid == book::PriceBook::UpdateResult::Ok);

    auto result2_bid = book_bid.increment(level2.price, level2.aggregate_shares);
    assert(result2_bid == book::PriceBook::UpdateResult::Ok);
    
    auto result3_bid = book_bid.increment(level3.price, level3.aggregate_shares);
    assert(result3_bid == book::PriceBook::UpdateResult::Ok);

    auto result4_bid = book_bid.increment(level4.price, level4.aggregate_shares);
    assert(result4_bid == book::PriceBook::UpdateResult::Ok);

    auto result5_bid = book_bid.increment(level5.price, level5.aggregate_shares);
    assert(result5_bid == book::PriceBook::UpdateResult::Ok);

    // ask.
    auto result1_ask = book_ask.increment(level1.price, level1.aggregate_shares);
    assert(result1_ask == book::PriceBook::UpdateResult::Ok);

    auto result2_ask = book_ask.increment(level2.price, level2.aggregate_shares);
    assert(result2_ask == book::PriceBook::UpdateResult::Ok);

    auto result3_ask = book_ask.increment(level3.price, level3.aggregate_shares);
    assert(result3_ask == book::PriceBook::UpdateResult::Ok);

    auto result4_ask = book_ask.increment(level4.price, level4.aggregate_shares);
    assert(result4_ask == book::PriceBook::UpdateResult::Ok);

    auto result5_ask = book_ask.increment(level5.price, level5.aggregate_shares);
    assert(result5_ask == book::PriceBook::UpdateResult::Ok);

    assert(book_bid.count() == 5);
    assert(book_ask.count() == 5);

    std::array<uint32_t, 5> ordered_levels{125000, 210000, 305000, 350000, 498000};

    for (int i = 0; i < 5; i++) {
        assert(book_ask.data()[i].price == ordered_levels[i]);
        assert(book_bid.data()[i].price == ordered_levels[4-i]);
    }
    
}

void test_increment_full() {

    book::PriceBook book{true};
    for (size_t i = 1; i <= book::PriceBook::CAPACITY; i++) {
        auto result = book.increment(i, i*2);
        assert(result == book::PriceBook::UpdateResult::Ok);
    }

    auto result = book.increment(500000, 200);
    assert(result == book::PriceBook::UpdateResult::Full);

    auto result1 = book.increment(1, 200);
    assert(result1 == book::PriceBook::UpdateResult::Ok);
    assert(book.data()[511].aggregate_shares == 202);

}

void test_decrement_partial() {

    book::PriceBook book{true};
    book::PriceLevel new_level{305000, 200};

    auto inserted = book.increment(new_level.price, new_level.aggregate_shares);
    assert(inserted == book::PriceBook::UpdateResult::Ok);

    auto shares_reduced = book.decrement(new_level.price, 100);
    assert(shares_reduced == book::PriceBook::UpdateResult::Ok);

    assert(book.count() == 1);
    assert(book.data()[0].price == new_level.price);
    assert(book.data()[0].aggregate_shares == 100);
    
}

void test_decrement_to_zero() {

    book::PriceBook book{true};
    book::PriceLevel level1{305000, 200};
    book::PriceLevel level2{610000, 100};
    book::PriceLevel level3{915000, 300};

    auto inserted1 = book.increment(level1.price, level1.aggregate_shares);
    assert(inserted1 == book::PriceBook::UpdateResult::Ok);

    auto inserted2 = book.increment(level2.price, level2.aggregate_shares);
    assert(inserted2 == book::PriceBook::UpdateResult::Ok);

    auto inserted3 = book.increment(level3.price, level3.aggregate_shares);
    assert(inserted3 == book::PriceBook::UpdateResult::Ok);

    assert(book.count() == 3);

    auto removed = book.decrement(level2.price, level2.aggregate_shares);
    assert(removed == book::PriceBook::UpdateResult::Ok);

    assert(book.count() == 2);

    assert(book.data()[0].price == level3.price);
    assert(book.data()[0].aggregate_shares == level3.aggregate_shares);

    assert(book.data()[1].price == level1.price);
    assert(book.data()[1].aggregate_shares == level1.aggregate_shares);

}

void test_decrement_not_found() {
    
    book::PriceBook book{true};
    book::PriceLevel level1{305000, 200};
    book::PriceLevel level2{610000, 100};

    auto result = book.increment(level1.price, level1.aggregate_shares);
    assert(result == book::PriceBook::UpdateResult::Ok);

    auto add_non_present_values = book.decrement(level2.price, level2.aggregate_shares);
    assert(add_non_present_values == book::PriceBook::UpdateResult::NotFound);

    assert(book.count() == 1);
    assert(book.data()[0].price == level1.price);
    assert(book.data()[0].aggregate_shares == level1.aggregate_shares);

}

void test_decrement_invalid() {

    book::PriceBook book{true};
    book::PriceLevel level1{305000, 200};   

    auto result = book.increment(level1.price, level1.aggregate_shares);
    assert(result == book::PriceBook::UpdateResult::Ok);

    auto result1 = book.decrement(level1.price, level1.aggregate_shares + 200);
    assert(result1 == book::PriceBook::UpdateResult::InvalidDecrement);

    assert(book.count() == 1);
    assert(book.data()[0].price == level1.price);
    assert(book.data()[0].aggregate_shares == level1.aggregate_shares);
}

int main(int argc, char** argv) {

    test_increment_new_level();
    test_increment_existing_level();
    test_increment_sorted_order();
    test_increment_full();
    test_decrement_partial();
    test_decrement_to_zero();
    test_decrement_not_found();
    test_decrement_invalid();

    puts("All Price Book tests passed.\n");
    return 0;
}