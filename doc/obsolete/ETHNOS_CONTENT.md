# Этносы и контент

**Файл собирается автоматически:** `python3 tools/ethnos_table.py`.
Правится не он, а `content/` — иначе таблица разойдётся с игрой (D100).

Народов: 4. Рецептов: 45. Строений: 26. Методов: 28.

## Сводка

| Народ | Страна | Сеет | Пасёт | Знает методов | Открыто рецептов | Открыто строений |
| --- | --- | --- | --- | --- | --- | --- |
| **Achaean** (`achaean`) | temperate | Einkorn, Flax | Sheep x 6 | 21 | 39/45 | 15/26 |
| **Northfolk** (`northfolk`) | temperate | Einkorn, Flax | Sheep x 4 | 19 | 33/45 | 13/26 |
| **Sumerian** (`sumerian`) | river_valley | Emmer, Flax | Sheep x 8 | 21 | 34/45 | 17/26 |
| **Steppe folk** (`yamna`) | river_valley | Emmer | Sheep x 6, Goat x 8 | 18 | 30/45 | 12/26 |

## Чем они отличаются

Методы, которые есть не у всех: это и есть разница между народами.

| Метод | Achaean | Northfolk | Sumerian | Steppe folk |
| --- | --- | --- | --- | --- |
| baking — Baking dough into bread | да | — | да | — |
| brickmaking — Moulding and drying mud brick | — | — | да | — |
| bronzeworking — Alloying bronze | семья | — | — | — |
| carpentry — Timber joining | да | да | — | семья |
| coppersmithing — Smelting and working copper | семья | семья | семья | семья |
| dairying — Making cheese from milk | да | — | да | да |
| feltmaking — Beating wool into felt | — | — | — | да |
| fishing — Taking fish from the river | — | — | да | — |
| masonry — Dressing and laying stone | семья | семья | семья | — |
| pottery — Shaping and firing clay | да | семья | да | — |
| reedwork — Cutting and binding reed | — | — | да | — |
| smoking — Keeping meat over a slow fire | — | да | — | — |
| thatching — Bundling reed and straw | да | да | — | да |
| weaving — Spinning and weaving fibre | да | да | да | семья |

«семья» — метод не общий, его держит одна семья из пула (`family_knowledge_pool`), и с её смертью он теряется.

## Achaean (`achaean`)

> A settled bronze-age people, not a band that must invent flintknapping. They arrive knowing how to sow, herd, weave, bake, throw pots and dress stone; smelting is rarer, held by a family or two.

- **Страна:** temperate
- **Язык:** achaean, **постройки:** aegean
- **Сеет:** Einkorn, Flax
- **Приходит со скотом:** Sheep x 6
- **Ремёсла:** farming, herding, woodcutting, mining, crafting, cooking, construction, hauling, foraging, hunting
- **Любимая еда:** Bread, Cheese, Roast mutton, Berries
- **Умения:** база 3, у мастера 7

**Знает (18):** baking, burial, carpentry, dairying, firemaking, flintknapping, foraging, grinding, hafting, herbcraft, herding, hunting, pottery, sewing, thatching, threshing, tillage, weaving

**Семейное знание (1 из пула):** bronzeworking, coppersmithing, masonry

**Открыто:** рецептов 39 из 45, строений 15 из 26.

**Не построит:** Courtyard wall, Felt tent, Gate, Granary, Irrigation canal, Lined store pit, Mud-brick house, Mud-brick house, large, Reed granary, Reed hut, Smokehouse

**Не умеет (методы):** brickmaking, feltmaking, reedwork, smoking

## Northfolk (`northfolk`)

> A forest people of the northern woods. Timber is the one thing they have in abundance and grain is the one thing they cannot count on, so they build in logs and keep their winter in a smokehouse rather than a granary. They hunt more than they sow, they keep a few sheep for wool rather than for meat, and their year is organised around not starving in February (D100).

- **Страна:** temperate
- **Язык:** northfolk, **постройки:** timber
- **Сеет:** Einkorn, Flax
- **Приходит со скотом:** Sheep x 4
- **Ремёсла:** hunting, woodcutting, foraging, crafting, construction, cooking, hauling, farming, herding, fishing
- **Любимая еда:** Smoked meat, Roast venison, Berries, Bread
- **Умения:** база 3, у мастера 7

**Знает (16):** burial, carpentry, firemaking, flintknapping, foraging, grinding, hafting, herbcraft, herding, hunting, sewing, smoking, thatching, threshing, tillage, weaving

**Семейное знание (1 из пула):** coppersmithing, masonry, pottery

**Открыто:** рецептов 33 из 45, строений 13 из 26.

**Не построит:** Bakery, Courtyard wall, Dairy shed, Felt tent, Gate, Granary, Irrigation canal, Lined store pit, Mud-brick bakery, Mud-brick house, Mud-brick house, large, Reed granary, Reed hut

**Не умеет (методы):** baking, brickmaking, bronzeworking, dairying, feltmaking, reedwork

## Sumerian (`sumerian`)

> A river-valley people of about 2000 BC. They build in sun-dried brick and reed because the floodplain gives them clay and marsh but almost no timber and no stone; copper comes in by trade, not out of the ground, so smelting is rare knowledge held by one family.

- **Страна:** river_valley
- **Язык:** emegir, **постройки:** mesopotamian
- **Сеет:** Emmer, Flax
- **Приходит со скотом:** Sheep x 8
- **Ремёсла:** farming, herding, crafting, fishing, cooking, construction, hauling, foraging, mining, hunting, scouting
- **Любимая еда:** Bread, Dates, Cheese, Roast mutton
- **Умения:** база 3, у мастера 7

**Знает (19):** baking, brickmaking, burial, dairying, firemaking, fishing, flintknapping, foraging, grinding, hafting, herbcraft, herding, hunting, pottery, reedwork, sewing, threshing, tillage, weaving

**Семейное знание (1 из пула):** coppersmithing, masonry

**Открыто:** рецептов 34 из 45, строений 17 из 26.

**Не построит:** Felt tent, Granary, Irrigation canal, Lean-to shelter, Palisade, Smokehouse, Storage pit, Timber house, Wattle hut

**Не умеет (методы):** bronzeworking, carpentry, feltmaking, smoking, thatching

## Steppe folk (`yamna`)

> A steppe herding people of the dry grass and thorn country. They live off the flock rather than off a field: milk and curd through the year, meat when there is a surplus, felt instead of woven cloth, and a tent instead of a house. They sow a little emmer where they can and think nothing of leaving it. Stone they know how to work, timber they mostly do not have, and their wealth walks about on four legs (D100).

- **Страна:** river_valley
- **Язык:** yamna, **постройки:** steppe
- **Сеет:** Emmer
- **Приходит со скотом:** Sheep x 6, Goat x 8
- **Ремёсла:** herding, hunting, foraging, crafting, cooking, hauling, construction, farming
- **Любимая еда:** Cheese, Roast mutton, Smoked meat, Berries
- **Умения:** база 3, у мастера 7

**Знает (15):** burial, dairying, feltmaking, firemaking, flintknapping, foraging, grinding, hafting, herbcraft, herding, hunting, sewing, thatching, threshing, tillage

**Семейное знание (1 из пула):** carpentry, coppersmithing, weaving

**Открыто:** рецептов 30 из 45, строений 12 из 26.

**Не построит:** Bakery, Brick kiln, Courtyard wall, Gate, Granary, Irrigation canal, Kiln, Lined store pit, Mud-brick bakery, Mud-brick house, Mud-brick house, large, Reed granary, Reed hut, Smokehouse

**Не умеет (методы):** baking, brickmaking, bronzeworking, pottery, reedwork, smoking

