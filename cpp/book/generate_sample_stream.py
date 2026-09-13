def pack_header(msg_type, stock_locate = 0, tracking_number = 0, timestamp = 0):
    # 1 byte for msg_type (its ASCII code)
    # 2 bytes big-endian for stock_locate
    # 2 bytes big-endian for tracking_number
    # 6 bytes big-endian for timestamp.

    msg_type_bytes        = msg_type.encode('ascii')
    stock_locate_bytes    = stock_locate.to_bytes(2, 'big')
    tracking_number_bytes = tracking_number.to_bytes(2, 'big')
    timestamp_bytes       = timestamp.to_bytes(6, 'big')

    result = (msg_type_bytes + stock_locate_bytes + tracking_number_bytes + timestamp_bytes)
    assert( len(result) == 11 )

    return result


def pack_stock_directory(stock_locate = 0):

    header            = pack_header('R', stock_locate=stock_locate)
    placeholder_bytes = b'\x00' * 28

    result = (header + placeholder_bytes)
    assert( len(result) == 39 )

    return result

def pack_add_order(stock_locate, order_ref = 0, is_buy = True, shares = 0, price = 0):

    header          = pack_header('A', stock_locate=stock_locate)
    order_ref_bytes = order_ref.to_bytes(8, 'big')
    is_buy_bytes    = 'B'.encode('ascii') if is_buy else 'S'.encode('ascii')
    shares_bytes    = shares.to_bytes(4, 'big')
    stock_bytes     = b'\x00' * 8
    price_bytes     = price.to_bytes(4, 'big')

    result = (header + order_ref_bytes + is_buy_bytes + shares_bytes + stock_bytes + price_bytes)
    assert( len(result) == 36 )

    return result

def pack_cancel(order_ref = 0, cancelled_shares = 0):

    header                 = pack_header('X')
    order_ref_bytes        = order_ref.to_bytes(8, 'big')
    cancelled_shares_bytes = cancelled_shares.to_bytes(4, 'big')

    result = (header + order_ref_bytes + cancelled_shares_bytes)
    assert( len(result) == 23 )

    return result

def pack_delete(order_ref = 0):

    header          = pack_header('D')
    order_ref_bytes = order_ref.to_bytes(8, 'big')

    result = (header + order_ref_bytes)
    assert( len(result) == 19 )

    return result

def frame(body):

    prefix = len(body).to_bytes(2, 'big')

    result = (prefix + body)
    return result

if __name__ == "__main__":

    stock_locate = 1

    stock_directory_message_bytes = frame(pack_stock_directory(stock_locate))

    add_order_message_bytes = frame(pack_add_order(stock_locate=1, order_ref=100, is_buy=True, shares=500, price=250000))

    cancel_message_bytes = frame(pack_cancel(order_ref=100, cancelled_shares=200))

    delete_message_bytes = frame(pack_delete(order_ref=100))

    stream = (stock_directory_message_bytes + add_order_message_bytes + cancel_message_bytes + delete_message_bytes)

    with open("sample_stream.bin", "wb") as file: file.write(stream)