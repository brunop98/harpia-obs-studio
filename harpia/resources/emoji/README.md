# Emoji pictures

`noto-emoji.pack` holds the 128 px colour emoji of Google's
[Noto Emoji](https://github.com/googlefonts/noto-emoji) (`2D/png/128`, plus the
flags from `third_party/region-flags/png`): 4012 pictures as WebP, for drawing
emoji in the editor's text clips (`editor/timeline/Emoji.hpp`).

The Noto Emoji image resources are licensed under the Apache License, Version
2.0 (`LICENSE-Apache-2.0.txt`); copyright Google LLC. Flag images come from
Noto's `third_party/region-flags` and are in the public domain or otherwise
exempt from copyright.

Rebuild it with `harpia/scripts/make-emoji-pack.py` (instructions inside).
