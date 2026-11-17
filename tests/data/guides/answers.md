# Guide answers, saved 2026-11-14

Real answers from the sites a guide asks, saved as they came. The expected values below were
read off the files by hand.

| File | Asked | What it is |
|---|---|---|
| info_637.json | HoYoLAB point/info, map 2, point 637 (a common chest in Mondstadt), en-us | a guide with text and a picture |
| info_missing.json | the same for point 999999999 | retcode -2010, data null: HoYoLAB has no such point |
| comments_3881.json | appsample comments for o17 / 3881 (point 637's pair), 30 a page | 30 comments, 13 with a picture, a summary |
| comments_empty.json | the same for docId 999999999 | no comments, an empty summary |
| google_3881.json | Google translate_a/t, sl=auto, tl=en, the 23 comments of 3881 not tagged en, most liked first | one [text, language] for each |
| google_one.json | Google translate_a/t, sl=auto, tl=en, the one text `сундук в кусте` | one [text, language] |
| mymemory_es.json | MyMemory, "no los ataques están en una cita", es to en | a translation |

## info_637.json
- text: `Hidden inside a bush (destroying it is optional).`
- picture: `https://upload-static.hoyoverse.com/wiki-ys-map/2021/08/01/43715711/52bd4be62c366cf8f0e9cf6ec3f155c7_5002326503694988461.png`

## comments_3881.json
- summary: `Chest is completely hidden inside a bush.`
- 30 comments, already most liked first: 432, 305, 147, 50, 39, 36, 34, 27, 22, 15, 11, 9, 7, 7, 7, 7,
  5 (x11), 3, 3, 3. The last one is by `L.E.`, says `Joey`, tagged es.
- The first: by `I.E.`, 2021-07-02, 432 likes, no lang tag, a picture.
- 13 have a picture on `https://game-cdn.appsample.com/gim/`: 12 under `comments/o17/3881/`, one
  `item-imgs/3881.jpg`.
- 7 are tagged en (305, 34, 11, 7, 5, 3 and 3 likes); the other 23 have no English text from appsample (its
  translations are ru, es, pt only), so they go to Google.
- The one with 22 likes is Russian: `Сундук спрятан в кусте`. The one with 9 likes: `сундук в кусте`.
- The one with 15 likes is tagged ru but says `chest bush`.

## google_3881.json
- 23 answers. Only two say ru: the 7th (`Chest hidden in a bush`, the 22-likes comment) and the
  9th (`chest in the bush`, the 9-likes one). All the others say en, the 15-likes "chest bush" too.

## mymemory_es.json
- translatedText: `No, the attacks are on a date.`

## google_one.json
- `[["chest in the bush","ru"]]`: one text asked comes back as a list of one, like many.
