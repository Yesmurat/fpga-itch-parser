/* 

Reference decoder for 9 in-scope NASDAQ TotalView-ITCH 5.0 message types.

Field layouts below are transcribed directly from NASDAQ's published
TotalView-ITCH 5.0 Interface Specification (v5.0, 03/06/2015), section 4.
This is the golden model itch_decoder.v is checked against. The two
implementations deliberately mirror each other's table shape (common
11-byte header, then a per-type list of (width, is_ascii) fields) so a
mistake in one is easy to spot against the other.

Wire conventions (spec section 3, "Data Types"): all integer fields are
big-endian unsigned; all alpha (ASCII) fields are left-justified,
space-padded on the right and carried byte-for-byte, no reversal; Price
fields are big-endian unsigned integers with 4 implied decimal places.

*/

#pragma once // make sure the header is included only once in a build.

#include <array>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <string>
#include <vector>

namespace itch {

constexpr int MAX_FIELDS = 14;                  // widest type is Stock Directory ('R')
constexpr int COMMON_HEADER_LEN = 11;           // type(1) + stock_locate(2) + tracking_number(2) + timestamp(6)

struct FieldSpec {                              // describe one field with within a message type

    uint8_t width;
    bool is_ascii;                              // if true -> raw bytes (ASCII); if false -> big-endian uint

};

struct TypeSpec {                               // describe an entire message type.

    uint8_t total_length;                       // 0 = unrecognized type byte
    uint8_t field_count;                        // fields after the common header
    std::array<FieldSpec, MAX_FIELDS> fields;   // 14 FieldSpec objects

};

inline const TypeSpec& type_spec(char t) {

    // Unknown Type.
    static const TypeSpec UNKNOWN{0, 0, 
        {

        }
    };

    // System Event.
    static const TypeSpec S {12, 1, 
        
        {
            {
                {1, true} // Event Code
            }
        }

    };

    // Stock Directory.
    static const TypeSpec R{39, 14,
        {
            {
                {8, true},  // Stock
                {1, true},  // Market Category
                {1, true},  // Financial Status Indicator
                {4, false}, // Round Lot Size
                {1, true},  // Round Lots Only
                {1, true},  // Issue Classification
                {2, true},  // Issue Sub-Type
                {1, true},  // Authenticity
                {1, true},  // Short Sale Threshold Indicator
                {1, true},  // IPO Flag
                {1, true},  // LULD Reference Price Tier
                {1, true},  // ETP Flag
                {4, false}, // ETP Leverage Factor
                {1, true}  // Inverse Indicator
            }
        }
    };

    // Add Order.
    static const TypeSpec A{36, 5, 
        {
            {
                {8, false}, // Order Reference Number
                {1, true},  // Buy/Sell Indicator
                {4, false}, // Shares
                {8, true},  // Stock
                {4, false} // Price
            }
        }
    };

    // Adder Order with MPID.
    static const TypeSpec F{40, 6, 
        {
            {
                {8, false}, // Order Reference Number
                {1, true},  // Buy/Sell Indicator
                {4, false}, // Shares
                {8, true},  // Stock
                {4, false}, // Price
                {4, true}  // Attribution (MPID)
            }
        }
    };

    // Order Executed.
    static const TypeSpec E{31, 3, 
        {
            {
                {8, false}, // Order Reference Number
                {4, false}, // Executed Shares
                {8, false} // Match Number
            }
        }
    };

    // Order Executed With Price.
    static const TypeSpec C{36, 5, 
        {
            {
                {8, false}, // Order Reference Number
                {4, false}, // Executed Shares
                {8, false}, // Match Number
                {1, true},  // Printable
                {4, false} // Execution Price
            }
        }
    };

    // Order Cancel.
    static const TypeSpec X{23, 2, 
        {
            {
                {8, false}, // Order Reference Number
                {4, false} // Cancelled Shares
            }
        }
    };

    // Order Delete.
    static const TypeSpec D{19, 1, 
        {
            {
                {8, false} // Order Reference Number
            }
        }
    };

    // Order Replace.
    static const TypeSpec U{35, 4, 
        {
            {
                {8, false}, // Original Order Reference Number
                {8, false}, // New Order Reference Number
                {4, false}, // Shares
                {4, false} // Price
            }
        }
    };

    switch (t) {
        case 'S': return S;
        case 'R': return R;
        case 'A': return A;
        case 'F': return F;
        case 'E': return E;
        case 'C': return C;
        case 'X': return X;
        case 'D': return D;
        case 'U': return U;
        default:  return UNKNOWN;
    }

}

struct DecodedMessage {

    uint64_t seq_num         = 0; // block index (0-based) within the input stream
    char     msg_type        = 0; // message type (UNKNOWN, S, R, A, etc.)

    uint16_t stock_locate    = 0;
    uint16_t tracking_number = 0; // NASDAQ's internal tracking ID
    uint64_t timestamp       = 0; // 48-bit value, nanoseconds since midnight
    int      field_count     = 0; // how many of 14 slots hold real data for this message's type

    std::array<uint64_t, MAX_FIELDS> field_int{};    // valid where the field is not ASCII
    std::array<std::string, MAX_FIELDS> field_str{}; // valid where the field is ASCII

    bool error_unknown_type    = false; // set when the type byte didn't match any of the 9 known types
    bool error_length_mismatch = false; // set when the type was recognized but the byte count handed to decode_one doesn't match what that type requires
    bool error_truncated       = false; // set when there is no byte available to read a type from

};

// Decode one message body (exactly len bytes at body, no length prefix).
inline DecodedMessage decode_one(const uint8_t* body, size_t len, uint64_t seq_num) {

    /* Decode Table.

        Byte Offset |       0      |      1-2     |       3-4       |   5-10    |
        Field       | Message Type | Stock Locate | Tracking Number | Timestamp |
        Width       |   1 byte     |   2 bytes    |     2 bytes     |  6 bytes  |

    */

    // Empty DecodedMessage with default values.
    DecodedMessage m;
    m.seq_num = seq_num;

    if (len < 1) {
        m.error_truncated = true;
        return m;
    }

    m.msg_type = static_cast<char>(body[0]); // from Decode Table.

    // Message type specification.
    const TypeSpec& spec = type_spec(m.msg_type);

    if (spec.total_length == 0) {
        m.error_unknown_type = true;
        return m;
    }

    if (len != spec.total_length) {
        m.error_length_mismatch = true;
        return m;
    }

    m.stock_locate = (static_cast<uint16_t>(body[1]) << 8) | body[2];    // from Decode Table.
    m.tracking_number = (static_cast<uint16_t>(body[3]) << 8) | body[4]; // from Decode Table.

    m.timestamp = 0;
    for (int i = 0; i < 6; ++i) {
        m.timestamp = (m.timestamp << 8) | body[5 + i];
    }

    size_t off = COMMON_HEADER_LEN;
    m.field_count = spec.field_count;

    for (int k = 0; k < spec.field_count; ++k) {

        // extract a particular field at fields[k] in spec.
        const FieldSpec& f = spec.fields[static_cast<size_t>(k)];

        if (f.is_ascii) { // if the field is ASCII text (like Stock, Buy/Sell Indicator, or Event Code)

            // add it to field_str array in a message.
            m.field_str[static_cast<size_t>(k)] = std::string(reinterpret_cast<const char*>(body + off), f.width);
                
        }
        
        else { // if the field is a number (like Shares, Price, or an Order Reference Number)

            uint64_t v = 0;

            for (int i = 0; i < f.width; ++i) {
                v = (v << 8) | body[off + static_cast<size_t>(i)];
            }

            m.field_int[static_cast<size_t>(k)] = v;

        }

        off += f.width;

    }

    return m;

}

/*
Reads a stream of [2-byte big-endian length][message bytes] blocks until
EOF. It's a format shared by a MoldUDP64 message block and a raw NASDAQ
historical ITCH sample file.
*/
inline std::vector<DecodedMessage> decode_all(std::istream& in) {

    std::vector<DecodedMessage> out;
    uint64_t seq = 0;

    while (true) {

        unsigned char len_prefix[2];
        in.read(reinterpret_cast<char*>(len_prefix), 2);
        auto bytes_read = in.gcount();

        if (bytes_read == 0) {
            break; // clean EOF between blocks
        }

        if (bytes_read != 2) {
            DecodedMessage m;
            m.seq_num = seq;
            m.error_truncated = true;
            out.push_back(m);
            break;
        }

        uint16_t len = (static_cast<uint16_t>(len_prefix[0]) << 8) | len_prefix[1];
        std::vector<uint8_t> body(len); // vectory "body" with "len" elements.

        if (len > 0) {
            // in.read(dest, n) -> read n bytes from in (the file or stdin) and write into dest (buffer).
            in.read( reinterpret_cast<char*>(body.data()) , len );
        }

        if ( static_cast<uint16_t>( in.gcount() ) != len ) {

            DecodedMessage m;
            m.seq_num = seq;
            m.error_truncated = true;
            out.push_back(m);
            break;

        }

        // body.data() -> where the raw bytes live
        // body.size() -> how many there are
        // seq         -> which message number is this
        out.push_back( decode_one( body.data(), body.size(), seq ) );
        ++seq;
    }

    return out;

}

} // namespace itch
