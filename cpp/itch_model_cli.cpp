/*
CLI around itch_model.hpp: decodes a [2B len][msg] block stream
(stdin, or a file path in argv[1]) and writes one line per message to
stdout, pipe-delimited with a fixed column count. Invoked as a subprocess
from sim/golden/itch_model.py. This binary is the actual golden model,
the Python wrapper just shells out to it and parses the output.

Column layout (fixed arity - always exactly MAX_FIELDS field columns,
blank past field_count):

Position |      Column      | Source                                     |
    1    | seq_num          | which message in the stream (0, 1, 2, ...) |
    2    | msg_type         | the type character (A, S, D, ...)          |
    3    | stock_locate     | common header                              |
    4    | tracking_number  | common header                              |
    5    | timestamp        | common header                              |
    6    | field_count      | how many of the next columns are real      |
   7-9   | error_* flags    | printed as 1/0                             |
  10-23  | field0...field13 | the type-specific fields, 14 slots always  |

*/

#include "itch_model.hpp"

#include <fstream>
#include <iostream>

namespace {

    void print_message(const itch::DecodedMessage& m) {

        std::cout   << m.seq_num << '|'
                    << (m.msg_type ? m.msg_type : ' ') << '|'
                    << m.stock_locate << '|'
                    << m.tracking_number << '|'
                    << m.timestamp << '|'
                    << m.field_count << '|'
                    << (m.error_unknown_type ? 1 : 0) << '|'
                    << (m.error_length_mismatch ? 1 : 0) << '|'
                    << (m.error_truncated ? 1 : 0);

        const itch::TypeSpec& spec = itch::type_spec(m.msg_type);

        for (int k = 0; k < itch::MAX_FIELDS; ++k) {

            std::cout << '|';

            if (k < m.field_count) {

                const itch::FieldSpec& f = spec.fields[static_cast<size_t>(k)]; // fetch fields array from the recipe.

                if (f.is_ascii) {
                    std::cout << m.field_str[static_cast<size_t>(k)];
                }

                else {
                    std::cout << m.field_int[static_cast<size_t>(k)];
                }

            }

        }

        std::cout << '\n';

    }

} // namespace

int main(int argc, char** argv) {

    std::vector<itch::DecodedMessage> messages;

    if (argc > 1) {

        std::ifstream f(argv[1], std::ios::binary);

        if (!f) {
            std::cerr << "itch_model_cli: cannot open " << argv[1] << '\n';
            return 1;
        }

        messages = itch::decode_all(f);

    }
    
    else {
        messages = itch::decode_all(std::cin);
    }

    for (size_t i = 0; i < messages.size(); ++i) {
        print_message(messages[i]);
    }

    return 0;
}
