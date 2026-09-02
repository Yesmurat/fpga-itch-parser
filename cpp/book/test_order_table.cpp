#include "order_table.hpp"
#include <cassert>
#include <cstdio>

void test_insert_and_find() {

    uint64_t order_ref = 12345; // just pick a value.
    book::Order order{0, true, 100, 1234500}; // symbol_index, is_buy, shares, price.

    book::OrderTable table; // object of class OrderTable.

    auto result = table.insert(order_ref, order);
    assert(result == book::OrderTable::InsertResult::Ok);

    book::Order* found = table.find(order_ref);
    assert(found != nullptr);
    assert(found->symbol_index == order.symbol_index);
    assert(found->is_buy       == order.is_buy);
    assert(found->shares       == order.shares);
    assert(found->price        == order.price);

}

void test_find_missing() {

    uint64_t order_ref = 12345;
    book::OrderTable table;

    auto not_found = table.find(order_ref);
    assert(not_found == nullptr);

}

void test_erase() {

    uint64_t order_ref = 12345;
    book::Order order {0, true, 100, 1234500};

    book::OrderTable table;

    auto result = table.insert(order_ref, order);
    assert(result == book::OrderTable::InsertResult::Ok);

    bool is_erased = table.erase(order_ref);
    assert(is_erased == true);

    book::Order* not_found = table.find(order_ref);
    assert(not_found == nullptr);

}

void test_erase_missing() {

    book::OrderTable table;
    uint64_t order_ref = 12345;

    bool is_erased = table.erase(order_ref);
    assert(is_erased == false);

}

void test_already_exists() {

    uint64_t order_ref = 12345;
    book::Order order1 {0, true, 100, 1234500};
    book::Order order2 {1, false, 150, 2345600};

    book::OrderTable table;

    auto result1 = table.insert(order_ref, order1);
    assert(result1 == book::OrderTable::InsertResult::Ok);

    auto result2 = table.insert(order_ref, order2);
    assert(result2 == book::OrderTable::InsertResult::AlreadyExists);

    book::Order* order1_found = table.find(order_ref);
    assert(order1_found != nullptr);
    assert(order1_found->is_buy == order1.is_buy);
    assert(order1_found->price == order1.price);
    assert(order1_found->shares == order1.shares);
    assert(order1_found->symbol_index == order1.symbol_index);

}

int main (int argc, char** argv) {

    test_insert_and_find();
    test_find_missing();
    test_erase();
    test_erase_missing();
    test_already_exists();

    puts("all Order Table tests passed.\n");
    return 0;
}