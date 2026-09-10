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

int main(int argc, char** argv) {

    test_add_order_creates_level();

    return 0;
}