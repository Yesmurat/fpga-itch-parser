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

int main (int argc, char** argv) {

    test_insert_and_find();
    test_find_missing();

    puts("all Order Table tests passed.\n");
    return 0;
}