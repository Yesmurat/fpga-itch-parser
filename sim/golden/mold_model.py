class MoldModel:
    def __init__(self, expected_seq=0):
        self.expected_seq = expected_seq # persists across packets
        self.session = None
        self.end_of_session = False

    def feed(self, packet):
        """One MoldUDP64 packet (the full UDP payload). Returns (messages, events)."""
        messages = []
        events = []

        # Header (offsets per spec: 0/10/18)
        session = packet[0:10]
        seq_num = int.from_bytes(packet[10:18], 'big') # 8 bytes, big-endian
        count   = int.from_bytes(packet[18:20], 'big') # 2 bytes, big-endian

        # Sequence Check (uses persisted state)
        if seq_num > self.expected_seq:
            events.append( (self.expected_seq, seq_num) ) # gap; still persists

        elif seq_num < self.expected_seq:
            return messages, events # duplicate/old: drop whole packet

        # Count Semantics
        if count == 0xFFFF: # end of session
            self.end_of_session = True
            self.expected_seq = seq_num # EoS carries next-expected
            return messages, events

        if count == 0:  # heartbeat: no messages, no advance
            self.expected_seq = seq_num # heartbeat carries next-expected; resync
            return messages, events

        # Message-Block Walk (offset 20 onward)
        off = 20
        for i in range(count):

            if off + 2 > len(packet):   # can't even read the length field
                events.append( ('truncated', seq_num + i) )
                break

            length = int.from_bytes( packet[off:off+2], 'big' )
            if off + 2 + length > len(packet):  # body runs past the buffer
                events.append( ('truncated', seq_num + i) )
                break

            body = packet[ off+2 : off+2+length ]
            type_byte = body[0] if length > 0 else None # zero-len legal (spec)
            messages.append( (seq_num + i, length, type_byte, body) )
            off += (2 + length)

        # Advance for next packet.
        self.expected_seq = seq_num + len(messages)
        return messages, events
