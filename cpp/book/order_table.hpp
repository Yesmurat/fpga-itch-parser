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

            }

            Order* find(uint64_t order_ref) { // nullptr if not found
                
            }

            bool erase(uint64_t order_ref) { // true if found and removed
                
            }

            const Order* find(uint64_t order_ref) const {

            }

        private:
            struct Slot {
                bool     occupied  = false;
                bool     tombstone = false;
                uint64_t order_ref = 0;
                Order    order{}; // The {} is a default member initializer.
            };
            
            std::array<Slot, CAPACITY> slots_{};
            size_t count_ = 0;

            static size_t hash(uint64_t order_ref) {

                uint64_t h = order_ref;
                h ^= (h >> 30);
                h *= 0xbf58476d1ce4e5b9;
                h ^= (h >> 27);
                h *= 0x94d049bb133111eb;
                h ^= (h >> 31);
                return static_cast<size_t>(h);
            }
            
            size_t probe(uint64_t order_ref) const { // returns the slot index for this key

            }

    };

} // namespace book