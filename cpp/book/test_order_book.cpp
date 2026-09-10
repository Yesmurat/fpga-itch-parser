#include "order_book.hpp"
#include "itch_decoder.hpp"
#include <cassert>

void test_add_order_creates_level () {

    book::OrderBook new_book;    // new object of an OrderBook class
    book::BookUpdate captured{}; // new BookUpdate struct

    bool got_update = false;
    
    auto callback = [&](const book::BookUpdate& u) {
        captured = u;
        got_update = true;
    }; // a lambda expression

    // assign the callback lambda expression to the private member callback_.
    new_book.set_callback(callback);

    itch::DecodedMessage message{};

    message.msg_type = 'A';     // message type.
    message.stock_locate = 10;  // tracking ID of this order.
    message.field_int[0] = 1;   // order reference.

    message.field_str[1] = 'B'; // buy
    message.field_int[2] = 100; // 100 shares
    message.field_int[4] = 15;  // for $15.

    new_book.apply(message);

    assert(got_update == true);
    assert(captured.symbol_index == 0);
    assert(captured.seq_num == 0);
    assert(captured.is_buy == true);
    assert(captured.best_shares == 100);
    assert(captured.best_price == 15);

}

void test_add_then_cancel_partial() {

    book::OrderBook new_book;
    book::BookUpdate captured{};

    bool got_update = false;

    auto callback = [&](const book::BookUpdate& u) {
        captured = u;
        got_update = true;
    };

    new_book.set_callback(callback);

    itch::DecodedMessage message_Add{};

    message_Add.msg_type = 'A';     // message_Add type.
    message_Add.stock_locate = 10;  // tracking ID of this order.
    message_Add.field_int[0] = 1;   // order reference.

    message_Add.field_str[1] = 'B'; // buy
    message_Add.field_int[2] = 200; // 100 shares
    message_Add.field_int[4] = 30;  // for $15.

    new_book.apply(message_Add);

    got_update = false;

    itch::DecodedMessage message_Cancel{};

    message_Cancel.msg_type = 'X';    // message_Cancel type.
    message_Cancel.stock_locate = 11; // tracking ID of this order.
    message_Cancel.field_int[0] = 1;  // order reference.
    message_Cancel.field_int[1] = 50; // the amount to cancel.

    new_book.apply(message_Cancel);

    assert(got_update == true);
    assert(captured.symbol_index == 0);
    assert(captured.is_buy == true);
    assert(captured.best_price == message_Add.field_int[4]);
    assert( captured.best_shares == (message_Add.field_int[2] - message_Cancel.field_int[1]) );

}

void test_add_then_delete() {

    book::OrderBook new_book;
    book::BookUpdate captured{};

    bool got_update = false;

    auto callback = [&](const book::BookUpdate& u) {
        captured = u;
        got_update = true;
    };

    new_book.set_callback(callback);

    itch::DecodedMessage message_Add{};

    message_Add.msg_type = 'A';     // message_Add type.
    message_Add.stock_locate = 10;  // tracking ID of this order.
    message_Add.field_int[0] = 1;   // order reference.

    message_Add.field_str[1] = 'B'; // buy
    message_Add.field_int[2] = 300; // 100 shares
    message_Add.field_int[4] = 45;  // for $15.

    new_book.apply(message_Add);

    got_update = false;

    itch::DecodedMessage message_Delete{};

    message_Delete.msg_type = 'D';
    message_Delete.field_int[0] = message_Add.field_int[0];

    new_book.apply(message_Delete);

    assert(got_update == true);
    assert(captured.best_price == 0);
    assert(captured.best_shares == 0);

}

void test_add_then_replace() {

    book::OrderBook new_book;
    book::BookUpdate captured{};

    bool got_update = false;

    auto callback = [&](const book::BookUpdate& u) {
        captured = u;
        got_update = true;
    };

    new_book.set_callback(callback);

    itch::DecodedMessage message_Add{};

    message_Add.msg_type = 'A';     // message_Add type.
    message_Add.stock_locate = 10;  // tracking ID of this order.
    message_Add.field_int[0] = 1;   // order reference.

    message_Add.field_str[1] = 'B'; // buy
    message_Add.field_int[2] = 300; // 300 shares
    message_Add.field_int[4] = 45;  // for $45.

    new_book.apply(message_Add);
    book::BookUpdate captured_after_Add = captured;

    got_update = false;

    itch::DecodedMessage message_Replace{};

    message_Replace.msg_type = 'U';

    message_Replace.field_int[0] = message_Add.field_int[0];
    message_Replace.field_int[1] = 2;
    message_Replace.field_int[2] = 130;
    message_Replace.field_int[3] = 17;

    new_book.apply(message_Replace);

    assert(got_update == true);
    assert(captured.symbol_index == captured_after_Add.symbol_index);
    assert(captured.is_buy == captured_after_Add.is_buy);
    assert(captured.best_shares == message_Replace.field_int[2]);
    assert(captured.best_price == message_Replace.field_int[3]);

}

void test_error_paths_dont_fire_callback() {

    book::OrderBook new_book;
    book::BookUpdate captured{};

    bool got_update = false;

    auto callback = [&](const book::BookUpdate& u) {
        captured = u;
        got_update = true;
    };

    new_book.set_callback(callback);

    // ----------------------------------------------------------------
    // Scenario 1: Unkown reference.
    itch::DecodedMessage message_Cancel{};

    message_Cancel.msg_type = 'X';
    message_Cancel.field_int[0] = 999; // order reference.
    message_Cancel.field_int[1] = 125; // shares.

    uint32_t orig_unknown_order_ref = new_book.report_stats().unknown_order_ref;

    new_book.apply(message_Cancel);

    assert(got_update == false);
    assert(new_book.report_stats().unknown_order_ref == (orig_unknown_order_ref + 1));

    // ----------------------------------------------------------------
    // Scenario 2: Invalid reduction.
    itch::DecodedMessage message_Add1{};

    message_Add1.msg_type = 'A';
    message_Add1.field_int[0] = 999; // order reference.
    message_Add1.field_int[2] = 50;  // shares.

    new_book.apply(message_Add1);

    assert(got_update == true);
    assert(captured.best_shares == message_Add1.field_int[2]);

    got_update = false;
    uint32_t orig_invalid_reduction = new_book.report_stats().invalid_reduction;

    new_book.apply(message_Cancel);

    assert(got_update == false);
    assert(new_book.report_stats().invalid_reduction == (orig_invalid_reduction + 1));

    // ----------------------------------------------------------------
    // Scenario 3: Duplicate insert.
    itch::DecodedMessage message_Add2{};

    message_Add2.msg_type = 'A';
    message_Add2.field_int[0] = 999; // order reference.
    message_Add2.field_int[2] = 25;  // shares.

    got_update = false;
    uint32_t orig_order_table_insert_failed = new_book.report_stats().order_table_insert_failed;

    new_book.apply(message_Add2);

    assert(got_update == false);
    assert(new_book.report_stats().order_table_insert_failed == (orig_order_table_insert_failed + 1));

}

int main(int argc, char** argv) {

    test_add_order_creates_level();
    test_add_then_cancel_partial();
    test_add_then_delete();
    test_add_then_replace();
    test_error_paths_dont_fire_callback();

    return 0;
}