import sys
import random

SEED        = 256                                # reproducibility
LOCATES     = [1, 2, 3, 4, 5, 6, 7, 8]           # array<SymbolBook,8>
BASE_PRICE  = 250_000                            # $25.0000 (4 implied decimals)
TICK        = 100                                # one cent
BAND        = 10                                 # ticks per side
WARMUP      = 500
TOTAL       = 50_000

# Message packers.
# ----------------------------------------------------------------
def pack_header(msg_type, stock_locate = 0, tracking_number = 0, timestamp = 0) -> bytes:
    # 1-byte msg_type (its ASCII code).
    # 2-byte big-endian stock_locate.
    # 2-byte big-endian tracking_number.
    # 6-byte big-endian timestamp.

    msg_type_bytes        = msg_type.encode('ascii')
    stock_locate_bytes    = stock_locate.to_bytes(2, 'big')
    tracking_number_bytes = tracking_number.to_bytes(2, 'big')
    timestamp_bytes       = timestamp.to_bytes(6, 'big')

    result = (msg_type_bytes + stock_locate_bytes + tracking_number_bytes + timestamp_bytes)
    assert( len(result) == 11 )

    return result

def pack_stock_directory(stock_locate = 0) -> bytes:

    header            = pack_header('R', stock_locate=stock_locate)
    placeholder_bytes = b'\x00' * 28

    result = (header + placeholder_bytes)
    assert( len(result) == 39 )

    return result

def pack_add(stock_locate, order_ref = 0, is_buy = True, shares = 0, price = 0) -> bytes:

    header          = pack_header('A', stock_locate=stock_locate)
    order_ref_bytes = order_ref.to_bytes(8, 'big')
    is_buy_bytes    = 'B'.encode('ascii') if is_buy else 'S'.encode('ascii')
    shares_bytes    = shares.to_bytes(4, 'big')
    stock_bytes     = b'\x00' * 8
    price_bytes     = price.to_bytes(4, 'big')

    result = (header + order_ref_bytes + is_buy_bytes + shares_bytes + stock_bytes + price_bytes)
    assert( len(result) == 36 )

    return result

def pack_cancel(order_ref = 0, cancelled_shares = 0) -> bytes:

    header                 = pack_header('X')
    order_ref_bytes        = order_ref.to_bytes(8, 'big')
    cancelled_shares_bytes = cancelled_shares.to_bytes(4, 'big')

    result = (header + order_ref_bytes + cancelled_shares_bytes)
    assert( len(result) == 23 )

    return result

def pack_delete(order_ref = 0) -> bytes:

    header          = pack_header('D')
    order_ref_bytes = order_ref.to_bytes(8, 'big')

    result = (header + order_ref_bytes)
    assert( len(result) == 19 )

    return result

def pack_replace(orig_order_ref = 0, new_order_ref = 0, shares = 0, price = 0) -> bytes:

    header = pack_header('U')
    orig_order_ref_bytes = orig_order_ref.to_bytes(8, 'big')
    new_order_ref_bytes  = new_order_ref.to_bytes(8, 'big')
    shares_bytes         = shares.to_bytes(4, 'big')
    price_bytes          = price.to_bytes(4, 'big')

    result = (header + orig_order_ref_bytes + new_order_ref_bytes + shares_bytes + price_bytes)
    assert( len(result) == 35 )

    return result

def frame(body : bytes) -> bytes:

    prefix = len(body).to_bytes(2, 'big')

    result = (prefix + body)
    return result
# ----------------------------------------------------------------

# Byte stream builders
# ----------------------------------------------------------------
def pool_add(order_table : dict, order_ref : int, locate : int, is_buy : bool, shares : int, price : int) -> None:
    
    assert order_ref not in order_table
    order_table[order_ref] = (locate, is_buy, shares, price)

def pool_remove(order_table : dict, order_ref : int) -> tuple:

    return order_table.pop(order_ref)

# returns a random order_ref from a list of "order_ref -> (locate, is_buy, shares, price)"s
def pool_pick(order_table : dict) -> int:

    assert order_table
    return random.choice(list(order_table))

# ----------------------------------------------------------------

def price_with_side_in_mind(is_buy):

    if is_buy:
        return BASE_PRICE - random.randint(1, BAND) * TICK   # should be 249000…249900
    else:
        return BASE_PRICE + random.randint(0, BAND-1) * TICK # should be 250000…250900

def build_large_stream(total=TOTAL, warmup=WARMUP, seed=SEED):

    random.seed(seed)
    order_table = {} # order_ref -> (locate, is_buy, shares, price)
    next_ref = 1
    parts = []       # a list of bytes objects, each object representing a separate message

    # 1. Add system messages first
    for locate in LOCATES:
        parts.append(frame(pack_stock_directory(locate)))

    def emit_add():    # append the Add message bytes to "parts" and record the order in "order_table".

        nonlocal next_ref

        locate = random.choice(LOCATES)             # which of the 8 stocks
        is_buy = random.choice([True, False])       # bid side or ask side
        price  = price_with_side_in_mind(is_buy)    # 249000-249900 or 250000-250900
        shares = random.randint(1, 10) * 100        # 100-1000, round lots

        # add to the list of bytes objects (messages)
        parts.append(
            frame(
                pack_add(locate, next_ref, is_buy, shares, price)
            )
        )

        # add to the live order table 
        pool_add(order_table, next_ref, locate, is_buy, shares, price)

        # increment the order reference
        next_ref += 1

    def emit_delete(): # remove an order from order_table and append a Delete message bytes to "parts"

        order_ref = pool_pick(order_table)
        pool_remove(order_table=order_table, order_ref=order_ref)

        parts.append(
            frame(
                pack_delete(order_ref=order_ref)
            )
        )

    def emit_cancel():

        order_ref = pool_pick(order_table)

        order_shares = order_table[order_ref][2]

        if order_shares < 200:

            # update "order_table"
            order_table.pop(order_ref)

            # update "parts"
            parts.append(frame(pack_delete(order_ref)))

            # finish
            return


        random_shares = random.randint(1, order_shares // 100 - 1) * 100

        # update "order_table"
        locate, is_buy, shares, price = order_table[order_ref]
        order_table[order_ref] = (locate, is_buy, shares - random_shares, price)

        # update "parts"
        parts.append(frame(pack_cancel(order_ref, random_shares)))

    def emit_replace():

        nonlocal next_ref

        old_order_ref = pool_pick(order_table)

        new_order_ref = next_ref
        next_ref += 1

        locate, is_buy, shares, price = order_table.pop(old_order_ref)

        new_price =  price_with_side_in_mind(is_buy)
        new_shares = random.randint(1, 10) * 100

        order_table[new_order_ref] = (locate, is_buy, new_shares, new_price)
        parts.append(
            frame(
                pack_replace(old_order_ref, new_order_ref, new_shares, new_price)
            )
        )

    # 2. Warmup
    for _ in range(warmup):
        emit_add()

    # 3. Filling "order_table" and "parts"
    for _ in range(total - warmup - len(LOCATES)):
        # Empty pool guard
        if not order_table:
            emit_add()
            
        else:
            # Weighted choice logic
            branch = random.choices(
                [emit_add, emit_cancel, emit_delete, emit_replace],
                weights=[40, 25, 20, 15]
            )[0]

            # Call the randomly picked function
            branch()

    return b"".join(parts)

def build_sample_stream():

    stock_directory_message_bytes = frame(pack_stock_directory(stock_locate=1))
    add_order_message_bytes       = frame(pack_add(stock_locate=1, order_ref=100, is_buy=True, shares=500, price=250000))
    cancel_message_bytes          = frame(pack_cancel(order_ref=100, cancelled_shares=200))
    delete_message_bytes          = frame(pack_delete(order_ref=100))

    stream = (stock_directory_message_bytes + add_order_message_bytes + cancel_message_bytes + delete_message_bytes)

    return stream

if __name__ == "__main__":

    if len(sys.argv) != 2:
        sys.stderr.write("usage: generate_stream.py {sample|large}\n")
        sys.exit(1)

    elif sys.argv[1] == "sample":
        data, path = build_sample_stream(), "sample_stream.bin"

    elif sys.argv[1] == "large":
        data, path = build_large_stream(), "large_stream.bin"

    else:
        sys.stderr.write("usage: generate_stream.py {sample|large}\n")
        sys.exit(1)

    with open(path, "wb") as file: file.write(data)