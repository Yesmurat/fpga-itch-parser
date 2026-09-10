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
    };

    class OrderBook {

        public:

            void set_callback(std::function<void(const BookUpdate)> cb) {
                callback_ = cb;
            }

            void apply(const itch::DecodedMessage& message) {

                switch (message.msg_type) {

                    case 'S': { // System Event.
                        /*
                        not about any specific order, but the whole session — markers like "start of messages," 
                        "start of market hours," "end of market hours," "end of messages." No book effect; 
                        it's just a timeline marker for session state, which is why the plan lists it as
                        a no-op in apply().
                        */
                        break;
                    }

                    case 'R': { // Stock Directory.


                        /*
                        establishes what a stock_locate number actually means — it links that 
                        numeric id to the real ticker (e.g., AAPL) plus some static info about the stock. Sent once per 
                        symbol near the start of the session, before any real orders for that symbol appear. This is exactly 
                        what you just wired up — it's the message that first teaches OrderBook "this locate number exists."
                        */

                        int idx = scan_locates(message.stock_locate);

                        if (idx == -1) {
                            locate_capacity_exceeded_++;
                        }

                        break;

                    }

                    case 'A': { // Add Order (No MPID).

                        /*
                        a brand-new limit order enters the book, anonymously — no 
                        attribution to the firm that placed it. Carries the order reference number, buy/sell side, share
                        count, stock locate, and price.
                        */

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

                        PriceBook& side_book = is_buy ? books_[idx].bids : books_[idx].asks;
                        side_book.increment(price, shares);

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

                    case 'F': { // Add Order (MPID Attached)

                        /*
                        functionally identical to A for book-keeping purposes, just 
                        with an extra field identifying the market participant (firm) that placed the order. That's why the 
                        plan groups A/F together — same book effect, different metadata.
                        */

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
                        side_book.increment(price, shares);

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

                    case 'E': { // Order Executed.

                        /*
                        some or all of an existing order's shares just traded, at the price it was 
                        originally displayed at. Carries the order reference and how many shares executed — nothing 
                        about price, since it doesn't change.
                        */

                        uint64_t order_ref       = message.field_int[0];
                        uint32_t executed_shares = message.field_int[1];
                        uint64_t match_number    = message.field_int[2];

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

                    case 'C': { // Order Executed With Price.

                        /*
                        the same idea as E, but the execution happened at a different 
                        price than what was displayed (this shows up for certain hidden/reserve order scenarios). For 
                        book-keeping, this behaves identically to E — the displayed price/size on the book still just 
                        reduces by the executed shares; the different execution price is a trade-reporting detail, not 
                        something that reprices the book.
                        */

                        uint64_t order_ref       = message.field_int[0];
                        uint32_t executed_shares = message.field_int[1];
                        uint64_t match_number    = message.field_int[2];
                        std::string printable    = message.field_str[3];
                        uint32_t execution_price = message.field_int[4];

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

                    case 'X': { // Order Cancel.

                        /*
                        a partial reduction — some of an order's remaining shares are cancelled, but 
                        the order itself might still be live afterward with fewer shares. Mechanically, same reduce-in-place 
                        logic as E.
                        */

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

                    case 'D': { // Order Delete.

                        /*
                        the entire remaining order is pulled from the book — no shares field at all, 
                        since deleting is total. This is the one that always removes the order from order_table_, not just
                        reduces it.
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

                    case 'U': { // Order Replace.

                        /*
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

                        if (orig_is_buy) {
                            books_[orig_symbol_index].bids.increment(new_price, new_shares);
                        }

                        else {
                            books_[orig_symbol_index].asks.increment(new_price, new_shares);
                        }

                        PriceBook& side_book = orig_is_buy ? books_[orig_symbol_index].bids : books_[orig_symbol_index].asks;

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
                    invalid_price_decrement_
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

            uint32_t locate_capacity_exceeded_  = 0;
            uint32_t order_table_insert_failed_ = 0;
            uint32_t unknown_order_ref_         = 0;
            uint32_t invalid_reduction_         = 0;
            uint32_t invalid_price_decrement_   = 0;

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