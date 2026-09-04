#pragma once
#include <cstddef>
#include <cstdint>
#include <array>

namespace book {

    struct PriceLevel {
        uint32_t price;
        uint32_t aggregate_shares;
    };

    class PriceBook {

        public:
            static constexpr size_t CAPACITY = 512;

            enum class UpdateResult {Ok, Full};

            PriceBook(bool is_buy_side) : is_buy_{is_buy_side} {}

            UpdateResult increment(uint32_t price, uint32_t shares) {

                int lowerbound = 0;
                int upperbound = count_ - 1;

                if (count_ != 0) {

                    while (lowerbound <= upperbound) {

                        int midPoint = lowerbound + (upperbound - lowerbound) / 2;
                    
                        if (levels_[midPoint].price == price) { // Target found at midPoint.

                            levels_[midPoint].aggregate_shares += shares;
                            return UpdateResult::Ok;

                        }
                    
                        else {

                            if (is_better(levels_[midPoint].price, price)) {
                                lowerbound = midPoint + 1;
                            }
                        
                            else {
                                upperbound = midPoint - 1;
                            }

                        }

                    }

                }

                // not found.
                if (count_ == CAPACITY) {
                    return UpdateResult::Full;
                }
                
                else {

                    for (int i = count_-1; i >= lowerbound; i--)
                        levels_[i+1] = levels_[i]; // shift the elements

                    // place the new level.
                    levels_[lowerbound] = {price, shares};

                    count_++;
                    return UpdateResult::Ok;

                }

            }

            UpdateResult decrement(uint32_t price, uint32_t shares) {}

        private:
            size_t count_ = 0; // how many of slots are actually in use; everything from count_ onward is stale/unused.
            const bool is_buy_;
            std::array<PriceLevel, CAPACITY> levels_{};

            bool is_better(uint32_t a, uint32_t b) const {

                if (is_buy_) {
                    return (a > b);
                }

                else {
                    return (a < b);
                }

            }

    };

}
