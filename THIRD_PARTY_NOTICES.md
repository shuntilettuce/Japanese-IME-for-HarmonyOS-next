# Third-Party Notices

This project ships derived data generated from the mozc project's
open-source dictionary and connection-cost data, augmented with
kanji-spelling vocabulary from JMdict and SudachiDict (see
`tools/mozc_data/build_mozc_engine.py`, `tools/mozc_data/
build_jmdict_augment.py`, and `tools/mozc_data/build_sudachi_augment.py`),
used to build the optional "統計データ" (mozc-derived) conversion engine,
which the user can enable alongside the app's own hand-built dictionary
via a settings toggle. The generated files
(`entry/src/main/resources/rawfile/mozc_dict.json`, `mozc_costs.json`,
`mozc_matrix.json`) are derived works of the data below and are covered by
the same license terms.

(An earlier round also fetched and merged in real mecab-ipadic's own
connection-cost matrix directly from https://github.com/taku910/mecab --
that merge was later superseded by using mozc's own matrix unreduced,
which measured better; see `tools/mozc_data/build_mozc_engine.py`'s module
docstring and git history. mozc's own bundled dictionary text is still
itself IPAdic-derived per mozc's own documentation, which is what the
"IPAdic-derived dictionary entries" section below covers -- that one is
unrelated to the now-removed direct fetch and still applies.)

The AI conversion engine's dictionary **shuntorge** (`kkc_lex.bin`, distributed
via GitHub Releases and fetched by `tools/fetch_ai_assets.py`) also contains
mozc's dictionary entries, connection costs, and symbol / emoji / emoticon
tables, plus readings from SudachiDict; the mozc, IPAdic, Okinawa Dictionary
and SudachiDict terms below apply to those parts as well. The AI conversion
model **shuntelligence** (`kkc_model.bin`) is licensed under CC BY 4.0
(attribution: "shuntelligence (shuntilettuce)"); the inference code in
`entry/src/main/cpp/` is MIT.

The English-word candidates in shuntorge (e.g. いんじぇくしょん → injection) were
checked for coverage against **The CEFR-J Wordlist Version 1.5**, compiled by
Yukio Tono, Tokyo University of Foreign Studies (copyright Tono Laboratory, TUFS;
free for research and commercial use with citation; retrieved via
https://github.com/openlanguageprofiles/olp-en-cefrj). The katakana readings
are this project's own.

shuntorge also contains:

- **Proper nouns from Japanese Wikipedia** (people, organizations, works,
  surnames / given names, and the original English titles of katakana
  article titles), extracted from the article leads ("題名（よみ）") of the
  jawiki dump of 2026-10-01 (https://dumps.wikimedia.org/jawiki/). Wikipedia
  text is licensed under **CC BY-SA 4.0**
  (https://creativecommons.org/licenses/by-sa/4.0/); attribution:
  "Wikipedia contributors, Japanese Wikipedia". Because of this, the
  dictionary file `kkc_lex.bin` as a whole is distributed under CC BY-SA 4.0
  as well (the same share-alike terms already apply through JMdict).
- **Readings of Latin-script words** (Win, GitHub, Excel …) from
  **mecab-ipadic-NEologd** (https://github.com/neologd/mecab-ipadic-neologd,
  `seed/mecab-user-dict-seed.20200910.csv.xz`), Copyright (C) 2015-2019
  Toshinori Sato (@overlast), under the **Apache License 2.0**. NEologd uses the
  spellings and readings of the Hatena Keyword list (株式会社はてな,
  http://developer.hatena.ne.jp/ja/documents/keyword/misc/catalog) and other
  sources listed in its COPYING file. Only Latin-script nouns that appear 30+
  times in FineWeb-2 Japanese were taken; whether the Latin spelling ranks first
  was decided from word counts in this project's training text.
- **Postal codes → addresses** from Japan Post's postal code data
  (`utf_ken_all.csv`, https://www.post.japanpost.jp/zipcode/). Japan Post
  states: "郵便番号データに限っては日本郵便株式会社は著作権を主張しません。
  自由に配布していただいて結構です。"
- **Emoji names** from the Unicode CLDR (cldr-json 48, Japanese
  `annotations` / `annotationsDerived`) and the order in Unicode's
  `emoji-test.txt` (18.0), under the **Unicode License v3**
  (https://www.unicode.org/license.txt). Copyright © 1991-2026 Unicode, Inc.
  Emoji were ranked by how often they appear in the FineWeb-2 Japanese test
  split (ODC-By 1.0); no FineWeb text is distributed.

Sources:
- https://github.com/google/mozc (branch: master, `src/data/dictionary_oss/`)
- https://www.edrdg.org/ (JMdict, via http://ftp.edrdg.org/pub/Nihongo/JMdict.gz)
  — used only to append extra kanji-spelling candidates to readings mozc's
  own dictionary already recognises (`tools/mozc_data/build_jmdict_augment.py`).
  See the "JMdict (CC BY-SA 4.0)" section below for the license text and
  the attribution/share-alike/update-mechanism terms this data carries.
- https://github.com/WorksApplications/SudachiDict (lexicon CSVs fetched
  from the project's own distribution bucket, see
  `tools/mozc_data/fetch_sudachi.py`) — used the same way as JMdict, only
  to append extra kanji-spelling candidates to readings mozc's own
  dictionary already recognises
  (`tools/mozc_data/build_sudachi_augment.py`). See the "SudachiDict
  (Apache License 2.0)" section below.

---

## mozc (connection cost data, POS definitions, and Google-authored
## dictionary additions) — BSD-3-Clause

```
Copyright 2010-2018, Google Inc.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

  * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
  * Redistributions in binary form must reproduce the above
    copyright notice, this list of conditions and the following disclaimer
    in the documentation and/or other materials provided with the
    distribution.
  * Neither the name of Google Inc. nor the names of its
    contributors may be used to endorse or promote products derived from
    this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## IPAdic-derived dictionary entries (files: `dictionary*.txt`) — NAIST /
## ICOT license (permissive)

```
Copyright 2000, 2001, 2002, 2003 Nara Institute of Science
and Technology.  All Rights Reserved.

Use, reproduction, and distribution of this software is permitted.
Any copy of this software, whether in its original form or modified,
must include both the above copyright notice and the following
paragraphs.

Nara Institute of Science and Technology (NAIST),
the copyright holders, disclaims all warranties with regard to this
software, including all implied warranties of merchantability and
fitness, in no event shall NAIST be liable for
any special, indirect or consequential damages or any damages
whatsoever resulting from loss of use, data or profits, whether in an
action of contract, negligence or other tortuous action, arising out
of or in connection with the use or performance of this software.

A large portion of the dictionary entries
originate from ICOT Free Software.  The following conditions for ICOT
Free Software applies to the current dictionary as well.

Each User may also freely distribute the Program, whether in its
original form or modified, to any third party or parties, PROVIDED
that the provisions of Section 3 ("NO WARRANTY") will ALWAYS appear
on, or be attached to, the Program, which is distributed substantially
in the same form as set out herein and that such intended
distribution, if actually made, will neither violate or otherwise
contravene any of the laws and regulations of the countries having
jurisdiction over the User or the intended distribution itself.

NO WARRANTY

The program was produced on an experimental basis in the course of the
research and development conducted during the project and is provided
to users as so produced on an experimental basis.  Accordingly, the
program is provided without any warranty whatsoever, whether express,
implied, statutory or otherwise.  The term "warranty" used herein
includes, but is not limited to, any warranty of the quality,
performance, merchantability and fitness for a particular purpose of
the program and the nonexistence of any infringement or violation of
any right of any third party.

Each user of the program will agree and understand, and be deemed to
have agreed and understood, that there is no warranty whatsoever for
the program and, accordingly, the entire risk arising from or
otherwise connected with the program is assumed by the user.

Therefore, neither ICOT, the copyright holder, or any other
organization that participated in or was otherwise related to the
development of the program and their respective officials, directors,
officers and other employees shall be held liable for any and all
damages, including, without limitation, general, special, incidental
and consequential damages, arising out of or otherwise in connection
with the use or inability to use the program or any product, material
or result produced or otherwise obtained by using the program,
regardless of whether they have been advised of, or otherwise had
knowledge of, the possibility of such damages at any time during the
project or thereafter.  Each user will be deemed to have agreed to the
foregoing by his or her commencement of use of the program.  The term
"use" as used herein includes, but is not limited to, the use,
modification, copying and distribution of the program and the
production of secondary products from the program.

In the case where the program, whether in its original form or
modified, was distributed or delivered to or received by a user from
any person, organization or entity other than ICOT, unless it makes or
grants independently of ICOT any specific warranty to the user in
writing, such person, organization or entity, will also be exempted
from and not be held liable to the user for any such damages as noted
above as far as the program is concerned.
```

## JMdict/EDICT (kanji-spelling candidate augmentation for the "統計データ"
## engine only, via `tools/mozc_data/build_jmdict_augment.py`) —
## CC BY-SA 4.0

Source: https://www.edrdg.org/ (The Electronic Dictionary Research and
Development Group), file fetched: http://ftp.edrdg.org/pub/Nihongo/JMdict.gz

License: Creative Commons Attribution-ShareAlike 4.0 International
(https://creativecommons.org/licenses/by-sa/4.0/). Full terms and the
EDRDG's own usage policy: https://www.edrdg.org/edrdg/licence.html

This project's use, and how it satisfies the license's conditions:
- **Attribution**: this notice, the in-app "ⓘ ライセンス" screen
  (`SymbolView.ets`), and `DATA_SOURCES.md` each credit JMdict/EDICT by
  name with a link to the EDRDG's project page, as required by both CC
  BY-SA 4.0 and the EDRDG's own stated policy for software/apps using
  these files.
- **ShareAlike**: only the derived, redistributed data
  (`entry/src/main/resources/rawfile/mozc_dict.json`'s appended candidate
  surfaces) is a JMdict-derived work; it is covered by this same CC BY-SA
  4.0 license, consistent with every other file in this repository being
  under a permissive/compatible license already (see the app code's own
  MIT license, noted in the in-app license screen).
- **"Regular updating" requirement**: the EDRDG's policy asks that
  software using JMdict data implement "a procedure for regular updating
  of the data from the most recent versions available." This project does
  not fetch JMdict at runtime (it is baked into a static shipped asset at
  build time, like every other data source here); the update procedure is
  to re-run `tools/mozc_data/fetch_jmdict.py` (which always re-downloads if
  the local cache is cleared) followed by
  `tools/mozc_data/build_jmdict_augment.py` before each release that
  touches track B's data, so the shipped snapshot doesn't go stale
  indefinitely.

Note: JMdict/EDICT was already used, separately, as one of several sources
for the hand-built `dict.json`/`global_dict.json` vocabulary (track A) --
see `DATA_SOURCES.md` and the in-app license screen's existing "変換辞書"
section. The usage documented here is a second, independent, later use of
the same source: appending extra kanji-spelling candidates to track B's
`mozc_dict.json` (see `tools/mozc_data/README.md`'s "JMdict vocabulary
augmentation" section for the mechanism and scope).

## SudachiDict (kanji-spelling candidate augmentation for the "統計データ"
## engine only, via `tools/mozc_data/build_sudachi_augment.py`) —
## Apache License 2.0

Source: https://github.com/WorksApplications/SudachiDict, copyright
2017-2023 Works Applications Co., Ltd. Lexicon CSVs fetched from the
project's own distribution host (see `tools/mozc_data/fetch_sudachi.py`
for the exact URLs, discovered via that host's public bucket listing).

License: Apache License, Version 2.0
(http://www.apache.org/licenses/LICENSE-2.0). SudachiDict's own README
states the project "incorporates UniDic and a part of NEologd" and
distributes the resulting lexicon as a whole under Apache-2.0, without
carving out separate terms for those incorporated components.

This project's use: only the "small" and "core" lexicon tiers (not the
much larger "notcore"/full tier of proper nouns) are scanned for entries
whose reading already exists as a key in `mozc_dict.json`; their kanji
surface is appended as an extra candidate for that reading, exactly like
the JMdict augmentation above (same "never adds a new reading key" safe
mode — see `build_sudachi_augment.py` and `build_jmdict_augment.py`'s
module docstrings, and `tools/mozc_data/README.md`, for why). Apache-2.0
is a permissive license (attribution required, no share-alike/copyleft
obligation on derivative data) — satisfied by this notice, the in-app "ⓘ
ライセンス" screen (`SymbolView.ets`), and `DATA_SOURCES.md`.

## Okinawa Dictionary entries (also within files: `dictionary*.txt`) —
## Public Domain

```
Public Domain Dataです。使用・変更・配布に関しては一切の制限をつけません。
商品などに組み込むことも自由に行なってください。すでにいくつかの辞書には沖縄辞書が採用されています。
勝手ながら、沖縄辞書に寄贈された辞書も in the Public Domain' 扱いとさせていただきます。
```

Source: http://sourceforge.jp/projects/o-dic/

## BIZ UDPGothic (bundled font: `entry/src/main/resources/rawfile/font/
## BIZUDPGothic-Regular.ttf`, also in the Android APK) — SIL Open Font License 1.1

The keyboard and candidate text use BIZ UDPGothic, a universal-design
Japanese font. The bundled file is a derived work: subset to the
characters the app can display, by `tools/build_ud_font.py`. Outlines are
unmodified.

```
Copyright 2022 The BIZ UDGothic Project Authors (https://github.com/googlefonts/morisawa-biz-ud-gothic)

This Font Software is licensed under the SIL Open Font License, Version 1.1.
(The license text is the same as in the Noto Sans JP section below.)
```

Source: https://github.com/google/fonts —
`ofl/bizudpgothic/BIZUDPGothic-Regular.ttf`

## Noto Sans JP (bundled font: `entry/src/main/resources/rawfile/font/
## NotoSansJP-Fallback.ttf`, also in the Android APK) — SIL Open Font License 1.1

The app bundles Japanese fonts because HarmonyOS's own system font ships
only Simplified/Traditional Chinese regional variants — without them,
Han-unified kanji render with Chinese glyph shapes on a China-region
device (see `entry/src/main/ets/components/JpFont.ets`).

This file supplies only the characters BIZ UDPGothic lacks (mostly rarer
kanji). It is a derived work of Noto Sans JP: instanced from the variable
font at `wght=400` and subset to those characters, by
`tools/build_ud_font.py`. Outlines are unmodified.

```
Copyright 2014-2021 Adobe (http://www.adobe.com/), with Reserved Font Name
'Source'. Copyright 2012 Google Inc. All Rights Reserved.

This Font Software is licensed under the SIL Open Font License, Version 1.1.
This license is copied below, and is also available with a FAQ at:
https://openfontlicense.org

Permission is hereby granted, free of charge, to any person obtaining a
copy of the Font Software, to use, study, copy, merge, embed, modify,
redistribute, and sell modified and unmodified copies of the Font
Software, subject to the following conditions:

1) Neither the Font Software nor any of its individual components, in
Original or Modified Versions, may be sold by itself.

2) Original or Modified Versions of the Font Software may be bundled,
redistributed and/or sold with any software, provided that each copy
contains the above copyright notice and this license. These can be
included either as stand-alone text files, human-readable headers or in
the appropriate machine-readable metadata fields within text or binary
files as long as those fields can be easily viewed by the user.

3) No Modified Version of the Font Software may use the Reserved Font
Name(s) unless explicit written permission is granted by the corresponding
Copyright Holder. This restriction only applies to the primary font name as
presented to the users.

4) The name(s) of the Copyright Holder(s) or the Author(s) of the Font
Software shall not be used to promote, endorse or advertise any Modified
Version, except to acknowledge the contribution(s) of the Copyright
Holder(s) and the Author(s) or with their explicit written permission.

5) The Font Software, modified or unmodified, in part or in whole, must be
distributed entirely under this license, and must not be distributed under
any other license. The requirement for fonts to remain under this license
does not apply to any document created using the Font Software.
```

Source: https://github.com/google/fonts —
`ofl/notosansjp/NotoSansJP[wght].ttf`

## 紅皿 (Benizara) の配列定義ファイル (files: `desktop/layouts/*.tsv` のうち、見出しに「紅皿 (Benizara) の配列定義ファイル … から書き起こした」とあるもの) — MIT 相当

キーと字の対応を https://github.com/k-ayaki/benizara の .bnz から書き起こした。各配列の考案者は各ファイルの見出し (#! name / #! url) を参照。

```
紅皿のLicense
Copyright (c) 2018 綾木健一郎

以下に定める条件に従い、IME.ahkおよび PATH.ahkを除く本ソフトウェアおよび関連文書のファイル（以下「ソフトウェア」）の複製を取得するすべての人に対し、ソフトウェアを無制限に扱うことを無償で許可します。これには、ソフトウェアの複製を使用、複写、変更、結合、掲載、頒布、サブライセンス、および/または販売する権利、およびソフトウェアを提供する相手に同じことを許可する権利も無制限に含まれます。

上記の著作権表示および本許諾表示を、ソフトウェアのすべての複製または重要な部分に記載するものとします。

ソフトウェアは「現状のまま」で、明示であるか暗黙であるかを問わず、何らの保証もなく提供されます。ここでいう保証とは、商品性、特定の目的への適合性、および権利非侵害についての保証も含みますが、それに限定されるものではありません。 作者または著作権者は、契約行為、不法行為、またはそれ以外であろうと、ソフトウェアに起因または関連し、あるいはソフトウェアの使用またはその他の扱いによって生じる一切の請求、損害、その他の義務について何らの責任も負わないものとします。
```
