// Thin CLI around itch_model.hpp: decodes a [2B len][msg] block stream
// (stdin, or a file path in argv[1]) and writes one line per message to
// stdout, pipe-delimited with a fixed column count. Invoked as a subprocess
// from sim/golden/itch_model.py -- this binary is the actual golden model,
// the Python wrapper just shells out to it and parses the output.
//
// Column layout (fixed arity -- always exactly MAX_FIELDS field columns,
// blank past field_count):
//   seq_num|msg_type|stock_locate|tracking_number|timestamp|field_count|
//   error_unknown_type|error_length_mismatch|error_truncated|
//   field0|field1|...|field13
//
// Pipe-delimited rather than CSV or JSON: no escaping semantics to get
// wrong (no ITCH field ever contains '|' or '\n'), and no new dependency
// (nlohmann/json) for a project that otherwise has zero C++ tooling.
// ASCII fields are emitted raw, including spec-mandated space-padding --
// the Python side decides whether to strip.
#include "itch_model.hpp"

#include <fstream>
#include <iostream>

namespace {

void print_message(const itch::DecodedMessage& m) {
    std::cout << m.seq_num << '|'
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
            const itch::FieldSpec& f = spec.fields[static_cast<size_t>(k)];
            if (f.is_ascii) std::cout << m.field_str[static_cast<size_t>(k)];
            else            std::cout << m.field_int[static_cast<size_t>(k)];
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
    } else {
        messages = itch::decode_all(std::cin);
    }

    for (const auto& m : messages) print_message(m);
    return 0;
}
