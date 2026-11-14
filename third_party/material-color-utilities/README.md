# material-color-utilities

The Material You look makes its colours from the Windows accent with Google's own
colour code, so they come out the way Android makes them.

- From https://github.com/material-foundation/material-color-utilities, commit
  194144d75ed7d34f06264a625e086db81a7ab9aa, the `cpp` folder.
- Only the files the tonal spot scheme needs are here (cam, contrast, dislike,
  dynamiccolor, palettes, scheme/scheme_tonal_spot, utils).
- Licensed under the Apache License 2.0, see LICENSE.
- One change: `utils/utils.cc` prints `HexFromArgb` with snprintf, so it needs no
  abseil.
