#!/usr/bin/env python3
"""Checks that the replay after a race drives the player's car exactly as the race did.

The 68000 keeps one byte of pad input per tick for the replay, from $FFC9A0 on, and
$FFC8C0 points at the next byte both while recording (the race) and while playing it back
(the replay), so it numbers the ticks on both sides. For each tick, the player's car
structure at the end of the tick's last frame is taken, once from the race and once from
the replay, and the two are compared.

usage: vrdetrace ... -m C8C0:C8C2 -m 9000:9100 rom.32x | python3 tools/replaycheck.py
  The race is the first pass of $FFC8C0 through the buffer; the replay is the last one,
  where it starts over at C9A0.
"""
import sys

POINTER = 0xC8C0
CAR = 0x9000
BUFFER_START = 0xC9A0
# Words that differ between the race and its replay from the first tick on, while every
# other word stays the same for the whole race: by their behaviour the replay's own (its
# cameras or display), not the driving. Their meaning has not been looked into.
REPLAY_ONLY = {0x9002, 0x90CA, 0x90CC}


def read_frames(lines):
    """Yields (frame, {address: word}) from vrdetrace's M lines"""
    frame, words = None, {}
    for line in lines:
        if not line.startswith('M '):
            continue
        parts = line.split()
        number = int(parts[1])
        if number != frame:
            if frame is not None:
                yield frame, words
            frame, words = number, {}
        address = int(parts[2], 16) & 0xFFFF
        for i, word in enumerate(parts[3:]):
            words[address + 2 * i] = int(word, 16)
    if frame is not None:
        yield frame, words


def car_words(words):
    return tuple(words[address] if address not in REPLAY_ONLY else 0
                 for address in sorted(words) if CAR <= address < CAR + 0x100)


def main():
    passes = []        # one {pointer: (frame, car)} per pass through the buffer
    last_pointer = None
    for frame, words in read_frames(sys.stdin):
        pointer = words.get(POINTER)
        if pointer is None or pointer < BUFFER_START:
            continue
        if last_pointer is None or pointer < last_pointer:
            passes.append({})
        passes[-1][pointer] = (frame, car_words(words))
        last_pointer = pointer
    if len(passes) < 2:
        print(f'found {len(passes)} pass(es) through the replay buffer; need the race and its replay')
        return 1
    race, replay = passes[0], passes[-1]
    ticks = sorted(set(race) & set(replay))
    print(f'race: {len(race)} ticks, replay: {len(replay)} ticks, compared: {len(ticks)}')
    for pointer in ticks:
        race_frame, race_car = race[pointer]
        replay_frame, replay_car = replay[pointer]
        if race_car != replay_car:
            different = [f'{CAR + 2 * i:04X}: {a:04X} {b:04X}'
                         for i, (a, b) in enumerate(zip(race_car, replay_car)) if a != b]
            print(f'first difference at tick {pointer - BUFFER_START} '
                  f'(race frame {race_frame}, replay frame {replay_frame}): '
                  + ', '.join(different[:8]))
            return 1
    print('the same on every tick')
    return 0


if __name__ == '__main__':
    sys.exit(main())
