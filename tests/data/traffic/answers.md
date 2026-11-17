# Game traffic test data

frames.bin is a real recording from game version 7.2, cut down for the tests:
- only the server's side, starting at the first message after the login, up to 30 s after it;
- every text field and every player UID is blanked (same length), then encrypted again with the
  session's real key, so the bytes still read like the game's;
- MAC addresses zeroed, our IP set to 10.0.0.2, UDP checksums 0.

Same format as the packet recorder: per frame a u64 unix ms, a u8 adapter, a u32 size, then the
ethernet frame (little endian).

The answers:
- 1600 frames, 685 whole messages.
- The first message starts at KCP sn 23, so a reader that waits for sn 0 gets nothing.
- The key: time seed 1793968363276, the first number of that seed's generator, server seed
  10378004310333966569. SearchKeyBetween from 60 s before to 60 s after the time seed finds it
  from the first message alone. With any other server seed it finds nothing.
- With that key all 685 messages decrypt to 45 67 ... 89 AB.
- ids.txt: the message ids in order (first 4384), as the reader decoded them before the trim.
