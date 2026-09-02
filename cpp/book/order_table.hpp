#pragma once
#include <cstddef>
#include <array>
#include <cstdint>

namespace book {

    struct Order
    {
        uint8_t  symbol_index; // 0-7, resolved from stock_locate at Add time
        bool     is_buy;
        uint32_t shares;       // remaining live shares
        uint32_t price;        // raw ITCH integer, 4 implied decimals
    };

    class OrderTable {

        public:
            static constexpr size_t CAPACITY = 65536;

            enum class InsertResult { Ok, Full, AlreadyExists };

            InsertResult insert(uint64_t order_ref, const Order& order) {

                if (count_ >= 0.9 * CAPACITY) {
                    return InsertResult::Full;
                }

                size_t idx = probe(order_ref);

                if (slots_[idx].occupied && slots_[idx].order_ref == order_ref) {
                    return InsertResult::AlreadyExists;
                }

                else {

                    slots_[idx] = {
                        .occupied = true,
                        .tombstone = false,
                        .order_ref = order_ref,
                        .order = order
                    };

                    count_++;
                    return InsertResult::Ok;

                }

            }

            Order* find(uint64_t order_ref) { // nullptr if not found

                size_t idx = probe(order_ref);

                if (slots_[idx].occupied && slots_[idx].order_ref == order_ref) {
                    return &slots_[idx].order;
                }

                else {
                    return nullptr;
                }
                
            }

            bool erase(uint64_t order_ref) { // true if found and removed

                size_t idx = probe(order_ref);

                if (!slots_[idx].occupied || slots_[idx].order_ref != order_ref) {
                    return false;
                }

                else {

                    slots_[idx].occupied  = false;
                    slots_[idx].tombstone = true;

                    count_--;
                    return true;

                }

            }

        private:
            struct Slot {
                bool     occupied  = false;
                bool     tombstone = false;
                uint64_t order_ref = 0;
                Order    order{}; // The {} is a default member initializer.
            };

            static size_t hash(uint64_t order_ref) {

                uint64_t h = order_ref;
                h ^= (h >> 30);
                h *= 0xbf58476d1ce4e5b9;
                h ^= (h >> 27);
                h *= 0x94d049bb133111eb;
                h ^= (h >> 31);
                return static_cast<size_t>(h);
            }
            
            std::array<Slot, CAPACITY> slots_{};
            size_t count_ = 0;
            
            size_t probe(uint64_t order_ref) const { // returns the slot index for this key

                bool found_tombstone = false;
                size_t tombstone_idx = 0;
                size_t i = hash(order_ref) & (CAPACITY - 1);

                for (size_t steps = 0 ; steps < CAPACITY; steps++, i = ((i + 1) & (CAPACITY - 1)) ) {

                    if (!slots_[i].occupied && !slots_[i].tombstone) {

                        if (found_tombstone) {
                            return tombstone_idx;
                        }
                        else {
                            return i;
                        }

                    }

                    else if (slots_[i].occupied && slots_[i].order_ref == order_ref) {
                        // return this index.
                        return i;
                    }

                    else if (slots_[i].occupied && slots_[i].order_ref != order_ref) {
                        continue;
                    }

                    else if (slots_[i].tombstone) {
                        // remember this as a candidate slot for insertion.

                        if (!found_tombstone) {
                            tombstone_idx = i;
                            found_tombstone = true;
                        }
                        
                        continue;
                    }

                }

            }

    };

} // namespace book