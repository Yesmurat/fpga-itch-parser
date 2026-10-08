#pragma once
#include <cassert>
#include "price_book.hpp"
#include "order_table.hpp"
#include "itch_decoder.hpp"
#include <cstddef>
#include <cstdint>
#include <array>
#include <functional>

namespace book {
    
    struct SymbolBook {
        PriceBook bids{true};
        PriceBook asks{false};
    };

    struct BookUpdate {
        uint8_t  symbol_index;
        bool     is_buy;
        uint32_t best_price;    // 0 if that side is now empty
        uint32_t best_shares;
        uint64_t seq_num;       // passed through from the triggering message
    };

    struct Stats {
            uint32_t locate_capacity_exceeded;
            uint32_t order_table_insert_failed;
            uint32_t unknown_order_ref;
            uint32_t invalid_reduction;
            uint32_t invalid_price_decrement;
            uint32_t price_level_capacity_exceeded;
            uint32_t malformed_message;
    };

    class OrderBook {

        public:

            void set_callback(std::function<void(const BookUpdate)> cb) {
                callback_ = cb;
            }

            void apply(const itch::DecodedMessage& message) {

                if (message.error_unknown_type || message.error_length_mismatch || message.error_truncated) {
                    malformed_message_++;
                    return;
                }

                switch (message.msg_type) {

                    case 'S': { // System Event Message.

                        // The system event message type is used to signal a market or data feed handler event.
                        // It's just a timeline marker for session state, which is why the plan lists it as a no-op in apply().
                        break;

                    }

                    // Stock Related Messages
                    case 'R': { // Stock Directory Message.

                        /*
                        At the start of each trading day, Nasdaw disseminates stock directory messages for all active symbols
                        in the Nasdaq execution system. It establishes what a stock_locate number actually means: it links that 
                        numeric id to the real ticker (e.g., AAPL) plus some static info about the stock.
                        
                        Sent once per symbol near the start of the session, before any real orders for that symbol appear.
                        This is exactly what you just wired up: it's the message that first teaches OrderBook "this locate number exists."
                        */

                        int idx = scan_locates(message.stock_locate);

                        if (idx == -1) {
                            locate_capacity_exceeded_++;
                        }

                        break;

                    }

                    /* Add Order Messages
                        An Add Order Message indicates that a new order has been accepted by the Nasdaq system and was added to the displayable book.
                        The message includes a day-unique Order Reference Number used by Nasdaq to track the order. Nasdaq will support two variations
                        of the Add Order message format.
                    */

                    case 'A': { // Add Order Message (No MPID).

                        // This message will be generated for unattributed orders accepted by the Nasdaq system.
                        // This message carries the order reference number, buy/sell side, share count, stock locate, and price.

                        int idx = scan_locates(message.stock_locate);

                        if (idx == -1) {
                            locate_capacity_exceeded_++;
                            break;
                        }

                        uint64_t order_ref   = message.field_int[0];
                        bool     is_buy      = (message.field_str[1][0] == 'B');
                        uint32_t shares      = message.field_int[2];
                        uint32_t price       = message.field_int[4];

                        Order order{static_cast<uint8_t>(idx), is_buy, shares, price};

                        auto result = order_table_.insert(order_ref, order);

                        if (result != OrderTable::InsertResult::Ok) {
                            order_table_insert_failed_++;
                            break;
                        }

                        PriceBook& side_book   = is_buy ? books_[idx].bids : books_[idx].asks;
                        auto result_increment = side_book.increment(price, shares);

                        if (result_increment != PriceBook::UpdateResult::Ok) {
                            order_table_.erase(order_ref);
                            price_level_capacity_exceeded_++;
                            break;
                        }

                        BookUpdate update {
                            order.symbol_index,
                            is_buy,
                            (side_book.count() > 0) ? side_book.data()[0].price : 0,
                            (side_book.count() > 0) ? side_book.data()[0].aggregate_shares : 0,
                            message.seq_num
                        };

                        if (callback_) callback_(update);
                        
                        break;
                    }

                    case 'F': { // Add Order Message (MPID Attached)

                        // This message will be generated for attributed orders and quotations accepted by the Nasdaq system.
                        // It's functionally identical to A for book-keeping purposes, just with an extra field identifying
                        // the market participant (firm) that placed the order.

                        int idx = scan_locates(message.stock_locate);

                        if (idx == -1) {
                            locate_capacity_exceeded_++;
                            break;
                        }

                        uint64_t order_ref      = message.field_int[0];
                        bool     is_buy         = (message.field_str[1][0] == 'B');
                        uint32_t shares         = message.field_int[2];
                        uint32_t price          = message.field_int[4];
                        std::string attribution = message.field_str[5];

                        Order order{static_cast<uint8_t>(idx), is_buy, shares, price};

                        auto result = order_table_.insert(order_ref, order);

                        if (result != OrderTable::InsertResult::Ok) {
                            order_table_insert_failed_++;
                            break;
                        }

                        PriceBook& side_book = is_buy ? books_[idx].bids : books_[idx].asks;
                        auto result_increment = side_book.increment(price, shares);

                        if (result_increment != PriceBook::UpdateResult::Ok) {
                            order_table_.erase(order_ref);
                            price_level_capacity_exceeded_++;
                            break;
                        }

                        BookUpdate update {
                            order.symbol_index,
                            is_buy,
                            (side_book.count() > 0) ? side_book.data()[0].price : 0,
                            (side_book.count() > 0) ? side_book.data()[0].aggregate_shares : 0,
                            message.seq_num
                        };

                        if (callback_) callback_(update);
                        
                        break;
                        
                    }

                    /* Modify Order Messages
                        Modify Order messages always include the Order Reference Number of the Add Order to which the update
                        applies. To determine the current display shares of an order, TICH subscribers must deduct the number of shares
                        stated in the Modiy message from the original number of shares stated in the Add Order message with the same
                        reference number. Nasdaq may send multiple Modify Order messages for the same order reference number and
                        the effects are cumilative. When the number of display shares for an order reaches zero, the order is dead and
                        should be removed from the book.
                    */

                    case 'E': { // Order Executed Message.

                        // This message is sent whenever an order on the book is executed in whole or in part. It is possible to receive several
                        // Order Executed Messages for the same order reference number if that order is executed in several parts. The
                        // multiple Order Executed Messages on the same order are cumilative.

                        // This message carries the order reference and how many shares executed; nothing about price, since it doesn't change.

                        uint64_t order_ref       = message.field_int[0];
                        uint32_t executed_shares = message.field_int[1];

                        Order* order = order_table_.find(order_ref);

                        if (order == nullptr) {
                            // not tracked.
                            unknown_order_ref_++;
                            break;
                        }

                        uint8_t symbol_index = order->symbol_index;
                        bool    is_buy       = order->is_buy;

                        // found.
                        if (executed_shares <= order->shares) {
                            order->shares -= executed_shares;
                        }
                        
                        else {
                            // report that executed shares exceed order's shares.
                            invalid_reduction_++;
                            break;
                        }

                        // decrement the price book using the order's own stored data.
                        if (order->is_buy) {

                            auto result = books_[order->symbol_index].bids.decrement(order->price, executed_shares);

                            if (result == PriceBook::UpdateResult::NotFound ||
                                result == PriceBook::UpdateResult::InvalidDecrement) {
                                    invalid_price_decrement_++;
                                    break;
                            }

                        }
                        
                        else {

                            auto result = books_[order->symbol_index].asks.decrement(order->price, executed_shares);

                            if (result == PriceBook::UpdateResult::NotFound ||
                                result == PriceBook::UpdateResult::InvalidDecrement) {
                                    invalid_price_decrement_++;
                                    break;
                                }

                        }

                        if (order->shares == 0) {
                            // the order is fully filled
                            order_table_.erase(order_ref);
                        }

                        PriceBook& side_book = is_buy ? books_[symbol_index].bids : books_[symbol_index].asks;

                        BookUpdate update {
                            symbol_index,
                            is_buy,
                            (side_book.count() > 0) ? side_book.data()[0].price : 0,
                            (side_book.count() > 0) ? side_book.data()[0].aggregate_shares : 0,
                            message.seq_num
                        };

                        if (callback_) callback_(update);

                        break;

                    }

                    case 'C': { // Order Executed With Price Message.

                        /*
                        This message is sent whenever an order on the book is executed in whole or in part at a price different from the 
                        initial display price. Since the execution price is different than the display price of the original Add Order, Nasdaq 
                        includes a price field within this execution message.

                        It is possible to receive multiple Order Executed and Order Executed With Price messages for the same order if that 
                        order is executed in several parts. The multiple Order Executed messages on the same order are cumulative.

                        For book-keeping, this behaves identically to E (the displayed price/size on the book still just 
                        reduces by the executed shares); the different execution price is a trade-reporting detail, not 
                        something that reprices the book.
                        */

                        uint64_t order_ref       = message.field_int[0];
                        uint32_t executed_shares = message.field_int[1];
                        std::string printable    = message.field_str[3];

                        Order* order = order_table_.find(order_ref);

                        if (order == nullptr) {
                            // not tracked.
                            unknown_order_ref_++;
                            break;
                        }

                        uint8_t symbol_index = order->symbol_index;
                        bool    is_buy       = order->is_buy;

                        // found.
                        if (executed_shares <= order->shares) {
                            order->shares -= executed_shares;
                        }
                        
                        else {
                            // report that executed shares exceed order's shares.
                            invalid_reduction_++;
                            break;
                        }

                        // decrement the price book using the order's own stored data.
                        if (order->is_buy) {

                            auto result = books_[order->symbol_index].bids.decrement(order->price, executed_shares);

                            if (result == PriceBook::UpdateResult::NotFound ||
                                result == PriceBook::UpdateResult::InvalidDecrement) {
                                    invalid_price_decrement_++;
                                    break;
                            }

                        }
                        
                        else {

                            auto result = books_[order->symbol_index].asks.decrement(order->price, executed_shares);

                            if (result == PriceBook::UpdateResult::NotFound ||
                                result == PriceBook::UpdateResult::InvalidDecrement) {
                                    invalid_price_decrement_++;
                                    break;
                                }

                        }

                        if (order->shares == 0) {
                            // the order is fully filled
                            order_table_.erase(order_ref);
                        }

                        PriceBook& side_book = is_buy ? books_[symbol_index].bids : books_[symbol_index].asks;

                        BookUpdate update {
                            symbol_index,
                            is_buy,
                            (side_book.count() > 0) ? side_book.data()[0].price : 0,
                            (side_book.count() > 0) ? side_book.data()[0].aggregate_shares : 0,
                            message.seq_num  
                        };

                        if (callback_) callback_(update);

                        break;
                    }

                    case 'X': { // Order Cancel Message.

                        // This message is sent whenever an order on the book is modified as a result of a partial cancellation.

                        uint64_t order_ref        = message.field_int[0];
                        uint32_t cancelled_shares = message.field_int[1];

                        Order* order = order_table_.find(order_ref);

                        if (order == nullptr) {
                            unknown_order_ref_++;
                            break;
                        }

                        uint8_t symbol_index = order->symbol_index;
                        bool    is_buy       = order->is_buy;

                        if (cancelled_shares <= order->shares) {
                            order->shares -= cancelled_shares;
                        }
                        
                        else {
                            invalid_reduction_++;
                            break;
                        }

                        if (order->is_buy) {

                            auto result = books_[order->symbol_index].bids.decrement(order->price, cancelled_shares);

                            if (result == PriceBook::UpdateResult::NotFound ||
                                result == PriceBook::UpdateResult::InvalidDecrement) {
                                    invalid_price_decrement_++;
                                    break;
                            }

                        }
                        
                        else {

                            auto result = books_[order->symbol_index].asks.decrement(order->price, cancelled_shares);

                            if (result == PriceBook::UpdateResult::NotFound ||
                                result == PriceBook::UpdateResult::InvalidDecrement) {
                                    invalid_price_decrement_++;
                                    break;
                            }

                        }

                        if (order->shares == 0) {
                            order_table_.erase(order_ref);
                        }

                        PriceBook& side_book = is_buy ? books_[symbol_index].bids : books_[symbol_index].asks;

                        BookUpdate update {
                            symbol_index,
                            is_buy,
                            (side_book.count() > 0) ? side_book.data()[0].price : 0,
                            (side_book.count() > 0) ? side_book.data()[0].aggregate_shares : 0,
                            message.seq_num  
                        };

                        if (callback_) callback_(update);

                        break;

                    }

                    case 'D': { // Order Delete Message.

                        /*
                        This message is sent whenever an order book is being cancelled. All remaining shares are no longer
                        accessible so the order must be removed from the book.

                        This message removes the order from order_table_, not just reduces it.
                        */

                        uint64_t order_ref = message.field_int[0];

                        Order* order = order_table_.find(order_ref);
                        if (order == nullptr) {
                            unknown_order_ref_++;
                            break;
                        }

                        uint8_t symbol_index = order->symbol_index;
                        bool    is_buy       = order->is_buy;

                        if (order->is_buy) {

                            auto result = books_[order->symbol_index].bids.decrement(order->price, order->shares);

                            if (result == PriceBook::UpdateResult::NotFound ||
                                result == PriceBook::UpdateResult::InvalidDecrement) {
                                    invalid_price_decrement_++;
                                    break;
                            }

                        }

                        else {

                            auto result = books_[order->symbol_index].asks.decrement(order->price, order->shares);

                            if (result == PriceBook::UpdateResult::NotFound ||
                                result == PriceBook::UpdateResult::InvalidDecrement) {
                                    invalid_price_decrement_++;
                                    break;
                            }

                        }

                        order_table_.erase(order_ref);

                        PriceBook& side_book = is_buy ? books_[symbol_index].bids : books_[symbol_index].asks;

                        BookUpdate update {
                            symbol_index,
                            is_buy,
                            (side_book.count() > 0) ? side_book.data()[0].price : 0,
                            (side_book.count() > 0) ? side_book.data()[0].aggregate_shares : 0,
                            message.seq_num
                        };

                        if (callback_) callback_(update);

                        break;

                    }

                    case 'U': { // Order Replace Message.

                        /*
                        This message is sent whenever an order book on the book has been cancel-replaced. All remaining shares from the
                        original order are no longer accessible, and must be removed. The new order details are provided for the
                        replacement, along with a new order reference number which will be used henceforth. Since the side, stock
                        symbol and attribution (if any) cannot be changed by an Order Replace event, these fields are not included in the
                        message. Firms should retain the side, stock symbol and MPID from the original Add Order message.

                        represents "modify an order" — but since ITCH has no in-place-modify 
                        message, it's expressed as one combined message: the old order reference is entirely retired, and 
                        a new order reference takes its place, typically at a different size and/or price. This is why 
                        apply()'s U handler needs to look up the original reference first (to recover which symbol/side 
                        it belonged to, since the message doesn't repeat that), remove its old contribution, then insert a 
                        fresh order under the new reference.
                        */
                       
                        uint64_t orig_order_ref = message.field_int[0];
                        uint64_t new_order_ref  = message.field_int[1];
                        uint32_t new_shares     = message.field_int[2];
                        uint32_t new_price      = message.field_int[3];

                        Order* order = order_table_.find(orig_order_ref);
                        if (order == nullptr) {
                            unknown_order_ref_++;
                            break;
                        }

                        uint8_t  orig_symbol_index = order->symbol_index;
                        bool     orig_is_buy       = order->is_buy;
                        uint32_t orig_price        = order->price;
                        uint32_t orig_shares       = order->shares;

                        if (orig_is_buy) {

                            auto result = books_[orig_symbol_index].bids.decrement(orig_price, orig_shares);

                            if (result == PriceBook::UpdateResult::NotFound ||
                                result == PriceBook::UpdateResult::InvalidDecrement) {
                                    invalid_price_decrement_++;
                                    break;
                            }

                        }

                        else {

                            auto result = books_[orig_symbol_index].asks.decrement(orig_price, orig_shares);

                            if (result == PriceBook::UpdateResult::NotFound ||
                                result == PriceBook::UpdateResult::InvalidDecrement) {
                                    invalid_price_decrement_++;
                                    break;
                            }

                        }

                        order_table_.erase(orig_order_ref);

                        Order new_order{orig_symbol_index, orig_is_buy, new_shares, new_price};
                        auto result = order_table_.insert(new_order_ref, new_order);

                        if (result != OrderTable::InsertResult::Ok) {
                            order_table_insert_failed_++;
                            break;
                        }

                        PriceBook& side_book = orig_is_buy ? books_[orig_symbol_index].bids : books_[orig_symbol_index].asks;

                        auto result1 = side_book.increment(new_price, new_shares);

                        if (result1 != PriceBook::UpdateResult::Ok) {
                            order_table_.erase(new_order_ref);
                            price_level_capacity_exceeded_++;
                        }

                        BookUpdate update {
                            orig_symbol_index,
                            orig_is_buy,
                            (side_book.count() > 0) ? side_book.data()[0].price : 0,
                            (side_book.count() > 0) ? side_book.data()[0].aggregate_shares : 0,
                            message.seq_num
                        };

                        if (callback_) callback_(update);
                        
                        break;

                    }

                    default: break;
                }

            }

            Stats report_stats() const {

                Stats stats{
                    locate_capacity_exceeded_,
                    order_table_insert_failed_,
                    unknown_order_ref_,
                    invalid_reduction_,
                    invalid_price_decrement_,
                    price_level_capacity_exceeded_,
                    malformed_message_
                };

                return stats;

            }

        private:
            std::array <SymbolBook, 8> books_;
            OrderTable order_table_;

            // raw locate values seen so far.
            std::array<uint16_t, 8> locates_{};

            // how many slots are filled.
            uint8_t num_locates_ = 0;

            // The position of a locate within locates_ is its symbol index:
            // locates_[3] == 1234 means locate 1234 maps to books_[3].

            std::function<void(const BookUpdate)> callback_;

            uint32_t locate_capacity_exceeded_      = 0;
            uint32_t order_table_insert_failed_     = 0;
            uint32_t unknown_order_ref_             = 0;
            uint32_t invalid_reduction_             = 0;
            uint32_t invalid_price_decrement_       = 0;
            uint32_t price_level_capacity_exceeded_ = 0;
            uint32_t malformed_message_             = 0;

            int scan_locates(uint16_t stock_locate) {

                int i = 0;

                for (; i < num_locates_; i++) {
                    if (locates_[i] == stock_locate)
                        return i;
                }

                // not found.
                if (num_locates_ < 8) {
                    locates_[num_locates_] = stock_locate;
                    num_locates_++;
                    return (num_locates_ - 1);
                }

                else {
                    return -1;
                }
                
            }

    };

}