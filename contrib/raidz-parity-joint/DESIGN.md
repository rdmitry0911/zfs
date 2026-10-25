# Raising RAID-Z parity in place: joint design

Working document for one design, written jointly by @rdmitry0911 and
@tigerblue77, before anything is proposed upstream. Discussion:
https://github.com/tigerblue77/zfs/issues/2

How this branch works:

- This file is changed only through pull requests into `parity-joint`,
  reviewed by the other author.
- Each prototype comes in as its own pull request, for side-by-side
  review. Prototypes are not merged here; only this document is.
- Anything stated as "mine does X" is "mine does X in that fixture"
  until the other side has run it.

## 1. Problem and constraints

Writes: tigerblue77. Reviews: rdmitry0911.

## 2. The two mechanisms

Writes: rdmitry0911; tigerblue77 corrects the column describing the
redirect prototype. Each reviews the other's column.

## 3. Decision points

Writes: tigerblue77. Reviews: rdmitry0911.

## 4. Measurements on one fixture

Each author for their own prototype; both review.

## 5. Landing order

Writes: tigerblue77. Reviews: rdmitry0911.
