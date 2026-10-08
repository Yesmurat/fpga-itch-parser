#include "itch_decoder.hpp"
#include <cassert>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

// Bodies built by hand, not via generate_stream.py: these tests need malformed bytes.

// Append an n-byte big-endian integer.
static void put_be(std::vector<uint8_t>& v, uint64_t value, int n) {
    for (int i = n - 1; i >= 0; --i)
        v.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFF));
}

// Append ASCII, left-justified and space-padded to width.
static void put_ascii(std::vector<uint8_t>& v, const std::string& s, size_t width) {
    for (size_t i = 0; i < width; ++i)
        v.push_back(i < s.size() ? static_cast<uint8_t>(s[i]) : ' ');
}

// Common header: type(1) locate(2) tracking(2) timestamp(6).
static std::vector<uint8_t> header(char type, uint16_t locate, uint16_t tracking, uint64_t timestamp) {
    std::vector<uint8_t> v;
    v.push_back(static_cast<uint8_t>(type));
    put_be(v, locate, 2);
    put_be(v, tracking, 2);
    put_be(v, timestamp, 6);
    return v;
}

// Append [2-byte length][body] to a stream buffer.
static void frame(std::vector<uint8_t>& out, const std::vector<uint8_t>& body) {
    put_be(out, body.size(), 2);
    out.insert(out.end(), body.begin(), body.end());
}

// std::string holds the embedded NULs.
static std::vector<itch::DecodedMessage> decode_bytes(const std::vector<uint8_t>& v) {
    std::string s(reinterpret_cast<const char*>(v.data()), v.size());
    std::istringstream in(s);
    return itch::decode_all(in);
}

// Smallest valid body, for framing-only tests.
static std::vector<uint8_t> body_delete(uint64_t order_ref) {
    std::vector<uint8_t> b = header('D', 1, 0, 0);
    put_be(b, order_ref, 8);
    return b;
}

// Spec table

// Literals and the width sum are both asserted: either alone misses a matched typo.
void test_type_spec_lengths() {

    struct Expected { char type; int total_length; int field_count; };

    const Expected table[] = {
        {'S', 12,  1}, {'R', 39, 14}, {'A', 36, 5},
        {'F', 40,  6}, {'E', 31,  3}, {'C', 36, 5},
        {'X', 23,  2}, {'D', 19,  1}, {'U', 35, 4}
    };

    for (const Expected& e : table) {

        const itch::TypeSpec& spec = itch::type_spec(e.type);

        assert(spec.total_length == e.total_length);
        assert(spec.field_count  == e.field_count);

        int width_sum = 0;
        for (int k = 0; k < spec.field_count; ++k)
            width_sum += spec.fields[static_cast<size_t>(k)].width;

        assert(itch::COMMON_HEADER_LEN + width_sum == spec.total_length);
    }
}

// total_length 0 = unsupported. 'P' and 'Q' are real types this project omits.
void test_type_spec_unknown() {

    for (char t : {'Z', 'P', 'Q', 'a', '\0'}) {
        const itch::TypeSpec& spec = itch::type_spec(t);
        assert(spec.total_length == 0);
        assert(spec.field_count  == 0);
    }
}

// Field placement. Both halves of each slot are asserted; the wrong one reads as 0 or "".

void test_decode_add_order() {

    std::vector<uint8_t> body = header('A', 7, 42, 0x0000ABCDEF01ull);
    put_be(body, 4471822, 8);   // slot 0  order reference  int
    put_ascii(body, "B", 1);    // slot 1  buy/sell         ascii
    put_be(body, 300, 4);       // slot 2  shares           int
    put_ascii(body, "AAPL", 8); // slot 3  stock            ascii
    put_be(body, 305000, 4);    // slot 4  price            int
    assert(body.size() == 36);

    itch::DecodedMessage m = itch::decode_one(body.data(), body.size(), 11);

    assert(m.msg_type        == 'A');
    assert(m.seq_num         == 11);
    assert(m.stock_locate    == 7);
    assert(m.tracking_number == 42);
    assert(m.timestamp       == 0x0000ABCDEF01ull);
    assert(m.field_count     == 5);
    assert(!m.error_unknown_type && !m.error_length_mismatch && !m.error_truncated);

    assert(m.field_int[0] == 4471822);
    assert(m.field_str[1] == "B");
    assert(m.field_int[2] == 300);
    assert(m.field_str[3] == "AAPL    ");
    assert(m.field_int[4] == 305000);

    // unused half of each slot
    assert(m.field_str[0].empty());
    assert(m.field_int[1] == 0);
    assert(m.field_str[2].empty());
    assert(m.field_int[3] == 0);
    assert(m.field_str[4].empty());

    // nothing past field_count
    assert(m.field_int[5] == 0);
    assert(m.field_str[5].empty());
}

// A plus a 4-byte ASCII attribution; the only 6-field type.
void test_decode_add_order_mpid() {

    std::vector<uint8_t> body = header('F', 3, 0, 1);
    put_be(body, 99, 8);
    put_ascii(body, "S", 1);
    put_be(body, 500, 4);
    put_ascii(body, "NVDA", 8);
    put_be(body, 250000, 4);
    put_ascii(body, "MPID", 4);  // slot 5  attribution  ascii
    assert(body.size() == 40);

    itch::DecodedMessage m = itch::decode_one(body.data(), body.size(), 0);

    assert(m.field_count == 6);
    assert(m.field_int[0] == 99);
    assert(m.field_str[1] == "S");
    assert(m.field_int[2] == 500);
    assert(m.field_str[3] == "NVDA    ");
    assert(m.field_int[4] == 250000);
    assert(m.field_str[5] == "MPID");
    assert(m.field_int[5] == 0);
}

// Widest type: 14 fields, and the only 2-byte ASCII field.
void test_decode_stock_directory() {

    std::vector<uint8_t> body = header('R', 1, 0, 0);
    put_ascii(body, "IBM", 8);  // 0  stock
    put_ascii(body, "Q", 1);    // 1  market category
    put_ascii(body, "N", 1);    // 2  financial status
    put_be(body, 100, 4);       // 3  round lot size        int
    put_ascii(body, "N", 1);    // 4  round lots only
    put_ascii(body, "C", 1);    // 5  issue classification
    put_ascii(body, "Z", 2);    // 6  issue sub-type        2 bytes
    put_ascii(body, "P", 1);    // 7  authenticity
    put_ascii(body, "N", 1);    // 8  short sale threshold
    put_ascii(body, "N", 1);    // 9  IPO flag
    put_ascii(body, "1", 1);    // 10 LULD tier
    put_ascii(body, "N", 1);    // 11 ETP flag
    put_be(body, 3, 4);         // 12 ETP leverage factor   int
    put_ascii(body, "N", 1);    // 13 inverse indicator
    assert(body.size() == 39);

    itch::DecodedMessage m = itch::decode_one(body.data(), body.size(), 0);

    assert(m.field_count == 14);
    assert(m.field_str[0]  == "IBM     ");
    assert(m.field_str[1]  == "Q");
    assert(m.field_int[3]  == 100);
    assert(m.field_str[6]  == "Z ");       // padded to its 2-byte width
    assert(m.field_int[12] == 3);
    assert(m.field_str[13] == "N");

    // slots stay disjoint
    assert(m.field_int[0] == 0);
    assert(m.field_str[3].empty());
    assert(m.field_str[12].empty());
}

// Four integer fields, no ASCII.
void test_decode_replace() {

    std::vector<uint8_t> body = header('U', 5, 9, 77);
    put_be(body, 100, 8);     // slot 0  original order reference
    put_be(body, 101, 8);     // slot 1  new order reference
    put_be(body, 300, 4);     // slot 2  shares
    put_be(body, 251000, 4);  // slot 3  price
    assert(body.size() == 35);

    itch::DecodedMessage m = itch::decode_one(body.data(), body.size(), 3);

    assert(m.field_count == 4);
    assert(m.field_int[0] == 100);
    assert(m.field_int[1] == 101);
    assert(m.field_int[2] == 300);
    assert(m.field_int[3] == 251000);

    for (int k = 0; k < 4; ++k)
        assert(m.field_str[static_cast<size_t>(k)].empty());
}

// Reference-only types: keyed on order_ref, no symbol or side.
void test_decode_reference_only_types() {

    // E - order executed: ref, executed shares, match number
    {
        std::vector<uint8_t> body = header('E', 1, 0, 0);
        put_be(body, 4471822, 8);
        put_be(body, 50, 4);
        put_be(body, 0xDEADBEEF, 8);
        assert(body.size() == 31);

        itch::DecodedMessage m = itch::decode_one(body.data(), body.size(), 0);
        assert(m.field_count == 3);
        assert(m.field_int[0] == 4471822);
        assert(m.field_int[1] == 50);
        assert(m.field_int[2] == 0xDEADBEEF);
    }

    // C - executed with price: slot 3 is ASCII, slot 4 the execution price
    {
        std::vector<uint8_t> body = header('C', 1, 0, 0);
        put_be(body, 500, 8);
        put_be(body, 25, 4);
        put_be(body, 7, 8);
        put_ascii(body, "Y", 1);
        put_be(body, 249900, 4);
        assert(body.size() == 36);

        itch::DecodedMessage m = itch::decode_one(body.data(), body.size(), 0);
        assert(m.field_count == 5);
        assert(m.field_int[0] == 500);
        assert(m.field_int[1] == 25);
        assert(m.field_int[2] == 7);
        assert(m.field_str[3] == "Y");
        assert(m.field_int[3] == 0);
        assert(m.field_int[4] == 249900);
    }

    // X - order cancel: ref, cancelled shares
    {
        std::vector<uint8_t> body = header('X', 1, 0, 0);
        put_be(body, 600, 8);
        put_be(body, 200, 4);
        assert(body.size() == 23);

        itch::DecodedMessage m = itch::decode_one(body.data(), body.size(), 0);
        assert(m.field_count == 2);
        assert(m.field_int[0] == 600);
        assert(m.field_int[1] == 200);
    }

    // D - order delete: ref only
    {
        std::vector<uint8_t> body = body_delete(700);
        assert(body.size() == 19);

        itch::DecodedMessage m = itch::decode_one(body.data(), body.size(), 0);
        assert(m.field_count == 1);
        assert(m.field_int[0] == 700);
    }
}

// Shortest type: one ASCII field.
void test_decode_system_event() {

    std::vector<uint8_t> body = header('S', 0, 0, 0);
    put_ascii(body, "O", 1);
    assert(body.size() == 12);

    itch::DecodedMessage m = itch::decode_one(body.data(), body.size(), 0);

    assert(m.msg_type == 'S');
    assert(m.field_count == 1);
    assert(m.field_str[0] == "O");
    assert(m.field_int[0] == 0);
}

// Wire conventions (spec section 3, "Data Types")

// Big-endian: first wire byte is most significant.
void test_big_endian_8_byte_field() {

    std::vector<uint8_t> body = header('D', 0, 0, 0);
    put_be(body, 0x0102030405060708ull, 8);

    itch::DecodedMessage m = itch::decode_one(body.data(), body.size(), 0);

    assert(m.field_int[0] == 0x0102030405060708ull);
    assert(m.field_int[0] == 72623859790382856ull);   // same value, decimal
}

// 48 bits in 6 bytes: no truncation to 32, no sign extension.
void test_timestamp_48_bit() {

    {   // all ones
        std::vector<uint8_t> body = body_delete(1);
        body[5] = body[6] = body[7] = body[8] = body[9] = body[10] = 0xFF;

        itch::DecodedMessage m = itch::decode_one(body.data(), body.size(), 0);
        assert(m.timestamp == 281474976710655ull);    // 2^48 - 1
    }

    {   // a value that would be lost if only the low 32 bits were read
        std::vector<uint8_t> body = header('D', 0, 0, 0x010000000000ull);
        put_be(body, 1, 8);

        itch::DecodedMessage m = itch::decode_one(body.data(), body.size(), 0);
        assert(m.timestamp == 0x010000000000ull);
        assert(m.timestamp != 0);
    }
}

// Never trimmed: apply()'s `field_str[1][0] == 'B'` depends on the padding surviving.
void test_ascii_padding_preserved() {

    std::vector<uint8_t> body = header('A', 1, 0, 0);
    put_be(body, 1, 8);
    put_ascii(body, "B", 1);
    put_be(body, 100, 4);
    put_ascii(body, "AAPL", 8);   // 4 chars into an 8-byte field
    put_be(body, 1, 4);

    itch::DecodedMessage m = itch::decode_one(body.data(), body.size(), 0);

    assert(m.field_str[3].size() == 8);
    assert(m.field_str[3] == "AAPL    ");
    assert(m.field_str[3][4] == ' ');
    assert(m.field_str[3] != "AAPL");   // not trimmed

    // exact fit, no padding
    assert(m.field_str[1].size() == 1);
    assert(m.field_str[1] == "B");
}

// Error paths. One flag each, and no field written - apply()'s guard relies on that.

void test_error_truncated() {

    std::vector<uint8_t> empty;
    itch::DecodedMessage m = itch::decode_one(empty.data(), 0, 4);

    assert(m.error_truncated);
    assert(!m.error_unknown_type);
    assert(!m.error_length_mismatch);
    assert(m.seq_num == 4);       // still stamped
    assert(m.msg_type == 0);      // never read
    assert(m.field_count == 0);
}

void test_error_unknown_type() {

    std::vector<uint8_t> body = header('Z', 1, 2, 3);

    itch::DecodedMessage m = itch::decode_one(body.data(), body.size(), 0);

    assert(m.error_unknown_type);
    assert(!m.error_truncated);
    assert(!m.error_length_mismatch);   // returns before the length check
    assert(m.msg_type == 'Z');          // the type byte was read
    assert(m.field_count == 0);
    assert(m.stock_locate == 0);        // header fields not yet parsed
}

void test_error_length_mismatch() {

    {   // too short: 'A' needs 36
        std::vector<uint8_t> body = header('A', 1, 0, 0);
        put_be(body, 1, 8);
        assert(body.size() == 19);

        itch::DecodedMessage m = itch::decode_one(body.data(), body.size(), 0);
        assert(m.error_length_mismatch);
        assert(!m.error_unknown_type);
        assert(!m.error_truncated);
        assert(m.msg_type == 'A');
        assert(m.field_count == 0);
    }

    {   // too long
        std::vector<uint8_t> body = header('D', 1, 0, 0);
        put_be(body, 1, 8);
        body.push_back(0xAA);          // 20 bytes; 'D' needs 19
        assert(body.size() == 20);

        itch::DecodedMessage m = itch::decode_one(body.data(), body.size(), 0);
        assert(m.error_length_mismatch);
        assert(m.field_count == 0);
    }
}

// No error path leaves a partially populated message.
void test_error_leaves_fields_default() {

    std::vector<uint8_t> empty;
    std::vector<uint8_t> unknown  = header('Z', 1, 2, 3);
    std::vector<uint8_t> mismatch = header('A', 1, 2, 3);

    itch::DecodedMessage messages[] = {
        itch::decode_one(empty.data(), 0, 0),
        itch::decode_one(unknown.data(), unknown.size(), 0),
        itch::decode_one(mismatch.data(), mismatch.size(), 0)
    };

    for (const itch::DecodedMessage& m : messages) {
        assert(m.error_truncated || m.error_unknown_type || m.error_length_mismatch);
        assert(m.field_count == 0);
        for (int k = 0; k < itch::MAX_FIELDS; ++k) {
            assert(m.field_int[static_cast<size_t>(k)] == 0);
            assert(m.field_str[static_cast<size_t>(k)].empty());
        }
    }
}

// decode_all() owns the length prefix and seq_num; decode_one() sees neither.

void test_decode_all_multiple_messages() {

    std::vector<uint8_t> stream;
    frame(stream, body_delete(100));
    frame(stream, body_delete(200));
    frame(stream, body_delete(300));

    std::vector<itch::DecodedMessage> out = decode_bytes(stream);

    assert(out.size() == 3);

    for (size_t i = 0; i < out.size(); ++i) {
        assert(out[i].seq_num == i);          // 0-based, one per block
        assert(out[i].msg_type == 'D');
        assert(!out[i].error_truncated);
    }

    assert(out[0].field_int[0] == 100);
    assert(out[1].field_int[0] == 200);
    assert(out[2].field_int[0] == 300);
}

// A block boundary at EOF is clean, not an error.
void test_decode_all_clean_eof() {

    std::vector<uint8_t> stream;
    frame(stream, body_delete(1));

    std::vector<itch::DecodedMessage> out = decode_bytes(stream);

    assert(out.size() == 1);                  // no trailing error entry
    assert(!out[0].error_truncated);
    assert(out[0].msg_type == 'D');

    assert(decode_bytes(std::vector<uint8_t>()).empty());   // empty input
}

// Length prefix cut in half at EOF.
void test_decode_all_truncated_prefix() {

    std::vector<uint8_t> stream;
    frame(stream, body_delete(1));
    stream.push_back(0x00);                   // one stray byte

    std::vector<itch::DecodedMessage> out = decode_bytes(stream);

    assert(out.size() == 2);
    assert(!out[0].error_truncated);
    assert(out[1].error_truncated);
    assert(out[1].seq_num == 1);
}

// Prefix claims more bytes than remain.
void test_decode_all_truncated_body() {

    std::vector<uint8_t> stream;
    put_be(stream, 36, 2);                    // claims an Add Order
    for (int i = 0; i < 10; ++i)              // but supplies only 10 bytes
        stream.push_back(0x41);

    std::vector<itch::DecodedMessage> out = decode_bytes(stream);

    assert(out.size() == 1);
    assert(out[0].error_truncated);
    assert(out[0].seq_num == 0);
}

// seq_num counts blocks, not successes.
void test_decode_all_seq_counts_blocks() {

    std::vector<uint8_t> unknown = header('Z', 1, 0, 0);

    std::vector<uint8_t> stream;
    frame(stream, unknown);                   // block 0: undecodable
    frame(stream, body_delete(900));          // block 1: fine

    std::vector<itch::DecodedMessage> out = decode_bytes(stream);

    assert(out.size() == 2);
    assert(out[0].error_unknown_type);
    assert(out[0].seq_num == 0);
    assert(out[1].msg_type == 'D');
    assert(out[1].seq_num == 1);              // not 0 - the bad block counted
    assert(out[1].field_int[0] == 900);
}

int main() {

    test_type_spec_lengths();
    test_type_spec_unknown();

    test_decode_add_order();
    test_decode_add_order_mpid();
    test_decode_stock_directory();
    test_decode_replace();
    test_decode_reference_only_types();
    test_decode_system_event();

    test_big_endian_8_byte_field();
    test_timestamp_48_bit();
    test_ascii_padding_preserved();

    test_error_truncated();
    test_error_unknown_type();
    test_error_length_mismatch();
    test_error_leaves_fields_default();

    test_decode_all_multiple_messages();
    test_decode_all_clean_eof();
    test_decode_all_truncated_prefix();
    test_decode_all_truncated_body();
    test_decode_all_seq_counts_blocks();

    std::printf("All ITCH decoder tests passed.\n");
    return 0;
}
