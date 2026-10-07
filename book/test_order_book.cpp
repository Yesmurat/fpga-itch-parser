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

void test_add_buy_order_then_replace() {

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

void test_add_sell_order_then_replace() {

    book::OrderBook new_book;
    book::BookUpdate captured{};

    bool got_update = false;

    auto callback = [&](const book::BookUpdate& u) {
        captured   = u;
        got_update = true;
    };

    new_book.set_callback(callback);

    itch::DecodedMessage message_Add{};

    message_Add.msg_type     = 'A'; // message_Add type.
    message_Add.field_int[0] = 1;   // original order reference.
    message_Add.field_str[1] = 'S'; // sell order.
    message_Add.field_int[2] = 300; // 300 shares
    message_Add.field_int[4] = 45;  // for $45.

    new_book.apply(message_Add);
    book::BookUpdate captured_after_Add = captured;

    got_update = false;

    itch::DecodedMessage message_Replace{};

    message_Replace.msg_type = 'U';
    message_Replace.field_int[0] = message_Add.field_int[0]; // order reference to replace.
    message_Replace.field_int[1] = 3;                        // new order reference.
    message_Replace.field_int[2] = 100;                      // 100 shares
    message_Replace.field_int[3] = 20;                       // for $20.

    new_book.apply(message_Replace);
    book::BookUpdate captured_after_Replace = captured;

    assert(got_update == true);
    assert(captured.is_buy == false);
    assert(captured.symbol_index == captured_after_Add.symbol_index);
    assert(captured.is_buy == captured_after_Add.is_buy);
    assert(captured.best_price == message_Replace.field_int[3]);
    assert(captured.best_shares == message_Replace.field_int[2]);

    got_update = false;

    itch::DecodedMessage message_Add1;

    message_Add1.msg_type     = 'A'; // message_Add type.
    message_Add1.field_int[0] = 5;   // order reference.
    message_Add1.field_str[1] = 'B'; // buy order.
    message_Add1.field_int[2] = 200; // 300 shares
    message_Add1.field_int[4] = 15;  // for $15.

    new_book.apply(message_Add1);
    assert(got_update == true);
    assert(captured.is_buy == true);
    assert(captured.best_shares == message_Add1.field_int[2]);
    assert(captured.best_price == message_Add1.field_int[4]);

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

void test_locate_capacity_exceeded() {

    book::OrderBook new_book{};
    book::BookUpdate captured{}; // new BookUpdate struct

    bool got_update = false;
    
    auto callback = [&](const book::BookUpdate& u) {
        captured = u;
        got_update = true;
    }; // a lambda expression

    new_book.set_callback(callback);

    itch::DecodedMessage message_Stock_Dir{};
    message_Stock_Dir.msg_type = 'R';

    for (uint16_t i = 100; i <= 107; i++) {
        message_Stock_Dir.stock_locate = i;
        new_book.apply(message_Stock_Dir);
        assert(new_book.report_stats().locate_capacity_exceeded == 0);
    }

    uint32_t orig_locate_capacity_exceeded = new_book.report_stats().locate_capacity_exceeded;

    itch::DecodedMessage message_Add{};
    message_Add.msg_type = 'A';

    message_Add.stock_locate = 108;
    new_book.apply(message_Add);

    assert(got_update == false);
    assert(new_book.report_stats().locate_capacity_exceeded == (orig_locate_capacity_exceeded + 1));

}

void test_multi_symbol_sanity_check() {

    book::OrderBook new_book;
    book::BookUpdate captured{}; // new BookUpdate struct

    bool got_update = false;
    
    auto callback = [&](const book::BookUpdate& u) {
        captured = u;
        got_update = true;
    }; // a lambda expression

    // assign the callback lambda expression to the private member callback_.
    new_book.set_callback(callback);

    itch::DecodedMessage message_A1{};
    message_A1.msg_type = 'A';
    message_A1.stock_locate = 200;
    message_A1.field_int[0] = 20;  // order reference.
    message_A1.field_int[2] = 400; // shares.
    message_A1.field_int[4] = 150; // price.

    new_book.apply(message_A1);
    uint8_t symbol_index_A1 = captured.symbol_index;

    itch::DecodedMessage message_A2{};
    message_A2.msg_type = 'A';
    message_A2.stock_locate = 300;
    message_A2.field_int[0] = 30;  // order reference.
    message_A2.field_int[2] = 600; // shares.
    message_A2.field_int[4] = 300; // price.

    new_book.apply(message_A2);
    uint8_t symbol_index_A2 = captured.symbol_index;

    assert(symbol_index_A1 != symbol_index_A2);

    itch::DecodedMessage message_X1{};
    message_X1.msg_type = 'X';
    message_X1.field_int[0] = message_A1.field_int[0];  // order reference.
    message_X1.field_int[1] = 200;                      // shares to cancel.

    new_book.apply(message_X1);
    assert(captured.symbol_index == symbol_index_A1);
    assert(captured.best_price == message_A1.field_int[4]);
    assert(captured.best_shares == (message_A1.field_int[2] - message_X1.field_int[1]));

    itch::DecodedMessage message_X2{};
    message_X2.msg_type = 'X';
    message_X2.field_int[0] = message_A2.field_int[0];  // order reference.
    message_X2.field_int[1] = 200;                      // shares to cancel.

    new_book.apply(message_X2);
    assert(captured.symbol_index == symbol_index_A2);
    assert(captured.best_price == message_A2.field_int[4]);
    assert(captured.best_shares == (message_A2.field_int[2] - message_X2.field_int[1]));

}

void test_price_level_capacity_exceeded() {
    
    book::OrderBook new_book;
    book::BookUpdate captured{};

    bool got_update = false;

    auto callback = [&](const book::BookUpdate& u) {
        captured = u;
        got_update = true;
    }; // a lambda expression

    new_book.set_callback(callback);

    size_t i = 0;
    for (; i < book::PriceBook::CAPACITY; i++) {

        itch::DecodedMessage message_Add{};
        message_Add.msg_type     = 'A';
        message_Add.field_int[0] = i;     // order reference.
        message_Add.stock_locate = 10;    // same stock locate across all messages.
        message_Add.field_str[1] = 'B';   // "Buy" order.
        message_Add.field_int[2] = 200;   // same number of shares across all messages.
        message_Add.field_int[4] = i*2;   // distinct price for each message.
        new_book.apply(message_Add);

    }

    assert(new_book.report_stats().price_level_capacity_exceeded == 0);

    got_update = false;
    uint32_t orig_price_level_capacity_exceeded  = new_book.report_stats().price_level_capacity_exceeded;
    uint32_t orig_order_table_insert_failed = new_book.report_stats().order_table_insert_failed;

    itch::DecodedMessage message_Add1{};
    message_Add1.msg_type     = 'A';
    message_Add1.field_int[0] = i + 20;                             // order reference.
    message_Add1.stock_locate = 10;                                 // stock locate.
    message_Add1.field_str[1] = 'B';                                // "Buy" order.
    message_Add1.field_int[2] = 200;                                // shares.
    message_Add1.field_int[4] = book::PriceBook::CAPACITY * 2 + 10; // price.
    new_book.apply(message_Add1);

    assert(got_update == false);
    assert(new_book.report_stats().price_level_capacity_exceeded == (orig_price_level_capacity_exceeded + 1));
    assert(new_book.report_stats().order_table_insert_failed == orig_order_table_insert_failed);

    uint32_t orig_unknown_order_ref       = new_book.report_stats().unknown_order_ref;
    uint32_t orig_invalid_price_decrement = new_book.report_stats().invalid_price_decrement;

    itch::DecodedMessage message_Cancel{};
    message_Cancel.msg_type = 'X';
    message_Cancel.stock_locate = 11; // stock locate.
    message_Cancel.field_int[0] = message_Add1.field_int[0];  // reference of an order whose shares to cancel.
    message_Cancel.field_int[1] = message_Add1.field_int[2]; // the amount of shares to cancel.
    new_book.apply(message_Cancel);

    assert( new_book.report_stats().unknown_order_ref == (orig_unknown_order_ref + 1) );
    assert(new_book.report_stats().invalid_price_decrement == orig_invalid_price_decrement);

}

void test_replace_onto_full_book() {

    book::OrderBook new_book;
    book::BookUpdate captured{};

    bool got_update = false;

    auto callback = [&](const book::BookUpdate& u) {
        captured = u;
        got_update = true;
    }; // a lambda expression

    new_book.set_callback(callback);

    size_t i = 0;
    for (; i < book::PriceBook::CAPACITY - 1; i++) {

        itch::DecodedMessage message_add{};

        message_add.msg_type     = 'A';
        message_add.stock_locate = 100;
        message_add.field_str[1] = 'B';    // Buy or Ask.
        message_add.field_int[0] = i + 1;  // orde ref.
        message_add.field_int[2] = 200;    // shares.
        message_add.field_int[4] = i + 1;  // price.

        new_book.apply(message_add);

    }

    itch::DecodedMessage message_a{};
    message_a.msg_type     = 'A';
    message_a.stock_locate = 100;
    message_a.field_str[1] = 'B';
    message_a.field_int[0] = 1000; // order ref.
    message_a.field_int[2] = 200;
    message_a.field_int[4] = i+1;
    new_book.apply(message_a);

    itch::DecodedMessage message_b{};
    message_b.msg_type     = 'A';
    message_b.stock_locate = 100;
    message_b.field_str[1] = 'B';
    message_b.field_int[0] = 1001; // order ref.
    message_b.field_int[2] = 200;
    message_b.field_int[4] = i+1;
    new_book.apply(message_b);

    // replace message_a
    itch::DecodedMessage message_u{};
    message_u.msg_type = 'U';
    message_u.field_int[0] = message_a.field_int[0]; // orig order ref.
    message_u.field_int[1] = 1002;                   // new order ref.
    message_u.field_int[2] = 250;                    // shares.
    message_u.field_int[3] = i+10;                   // price.

    got_update = false;
    uint32_t orig_price_level_capacity_exceeded = new_book.report_stats().price_level_capacity_exceeded;
    uint32_t orig_order_table_insert_failed     = new_book.report_stats().order_table_insert_failed;

    new_book.apply(message_u);

    assert(got_update == true);
    assert(new_book.report_stats().price_level_capacity_exceeded == orig_price_level_capacity_exceeded + 1);
    assert(new_book.report_stats().order_table_insert_failed == orig_order_table_insert_failed);

    itch::DecodedMessage message_x1{};
    message_x1.msg_type = 'X';
    message_x1.field_int[0] = message_u.field_int[1]; // order ref.
    message_x1.field_int[1] = 200;

    uint32_t orig_unknown_order_ref = new_book.report_stats().unknown_order_ref;
    new_book.apply(message_x1);
    assert(new_book.report_stats().unknown_order_ref == orig_unknown_order_ref + 1);

    itch::DecodedMessage message_x2{};
    message_x2.msg_type = 'X';
    message_x2.field_int[0] = message_a.field_int[0];
    message_x2.field_int[1] = 150;

    orig_unknown_order_ref = new_book.report_stats().unknown_order_ref;
    new_book.apply(message_x2);
    assert(new_book.report_stats().unknown_order_ref == orig_unknown_order_ref + 1);

}

int main(int argc, char** argv) {

    test_add_order_creates_level        ();
    test_add_then_cancel_partial        ();
    test_add_then_delete                ();
    test_add_buy_order_then_replace     ();
    test_add_sell_order_then_replace    ();
    test_error_paths_dont_fire_callback ();
    test_locate_capacity_exceeded       ();
    test_multi_symbol_sanity_check      ();
    test_price_level_capacity_exceeded  ();
    test_replace_onto_full_book         ();

    return 0;
}