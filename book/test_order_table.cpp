#include "order_table.hpp"
#include <cassert>
#include <cstdio>
#include <unordered_map>

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

size_t local_hash(uint64_t order_ref) {

    uint64_t h = order_ref;

    h ^= (h >> 30);
    h *= 0xbf58476d1ce4e5b9;
    h ^= (h >> 27);
    h *= 0x94d049bb133111eb;
    h ^= (h >> 31);

    return static_cast<size_t>(h);

}

void test_collision() {

    const int SIZE = 1000;

    // declare key-value pairs.
    std::unordered_map<size_t, uint64_t> pairs{};

    uint64_t collided_key1, collided_key2;
    bool found_collision = false;

    // populate key-value pairs.
    for (uint64_t key = 1; key < SIZE; key++) {

        size_t home = local_hash(key) & (book::OrderTable::CAPACITY - 1);

        if (pairs.count(home)) {

            // home is already claimed by pairs[home] -> collision,
            // paired with the current key. Stop here.
            collided_key1 = pairs[home];
            collided_key2 = key;
            found_collision = true;
            break;

        }

        else {
            pairs[home] = key;
        }

    }

    assert(found_collision);

    book::OrderTable table;
    book::Order order1 {0, true, 100, 1234500};
    book::Order order2 {1, false, 150, 2345600};
    
    auto result1 = table.insert(collided_key1, order1);
    assert(result1 == book::OrderTable::InsertResult::Ok);
    
    auto result2 = table.insert(collided_key2, order2);
    assert(result2 == book::OrderTable::InsertResult::Ok);

    book::Order* found1 = table.find(collided_key1);
    assert(found1 != nullptr);
    assert(found1->symbol_index == order1.symbol_index);
    assert(found1->is_buy == order1.is_buy);
    assert(found1->shares == order1.shares);
    assert(found1->price == order1.price);

    book::Order* found2 = table.find(collided_key2);
    assert(found2 != nullptr);
    assert(found2->symbol_index == order2.symbol_index);
    assert(found2->is_buy == order2.is_buy);
    assert(found2->shares == order2.shares);
    assert(found2->price == order2.price);
}

void test_capacity_full() {

    book::OrderTable table;
    book::Order order {0, true, 100, 1234500};

    size_t threshold = 58983; // for CAPACITY = 65536 -> threshold ~= 58983

    for (size_t order_ref = 1; order_ref <= threshold; order_ref++) {

        auto result = table.insert(order_ref, order);
        assert(result == book::OrderTable::InsertResult::Ok);

    } // populate the table up to 90% of its stated capacity.

    // insert one more key -> should return full.
    auto result1 = table.insert( (uint64_t)(threshold+1), order );
    assert(result1 == book::OrderTable::InsertResult::Full);

    auto result2 = table.insert( (uint64_t)(threshold+2), order );
    assert(result2 == book::OrderTable::InsertResult::Full);
    
}

void test_sustained_reuse() {

    uint64_t loop_count_max = 10 * book::OrderTable::CAPACITY;
    book::OrderTable table;
    book::Order order {0, true, 100, 1234500};

    for (uint64_t order_ref = 1; order_ref <= loop_count_max; order_ref++) {

        auto is_inserted = table.insert(order_ref, order);
        assert(is_inserted == book::OrderTable::InsertResult::Ok);

        book::Order* found1 = table.find(order_ref);
        assert(found1 != nullptr);
        assert(found1->is_buy == order.is_buy);
        assert(found1->price == order.price);
        assert(found1->shares == order.shares);
        assert(found1->symbol_index == order.symbol_index);

        bool erased = table.erase(order_ref);
        assert(erased == true);

        book::Order* found2 = table.find(order_ref);
        assert(found2 == nullptr);

    }

}

int main (int argc, char** argv) {

    test_insert_and_find();
    test_find_missing();
    test_erase();
    test_erase_missing();
    test_already_exists();
    test_collision();
    test_capacity_full();
    test_sustained_reuse();

    puts("all Order Table tests passed.\n");
    return 0;
}