"""Write doc/ETHNOS_CONTENT.md: what each people is made of.

Generated rather than written by hand, because a table of content that is kept
by hand is a table that is wrong by the second week. Run it after touching
anything under content/:

    python3 tools/ethnos_table.py

It reads the JSON straight, the same files the game reads, and prints what each
culture arrives knowing, what it sows, what it herds, what it eats, and - worked
out from the knowledge - which recipes and buildings are actually open to it.
"""
import json
import pathlib
from collections import OrderedDict

ROOT = pathlib.Path(__file__).resolve().parent.parent
CONTENT = ROOT / 'content'


def load(folder):
    out = []
    for path in sorted((CONTENT / folder).glob('*.json')):
        data = json.loads(path.read_text())
        for entry in (data if isinstance(data, list) else [data]):
            entry['_file'] = path.name
            out.append(entry)
    return out


def label(entry):
    return entry.get('label') or entry.get('name')


def main():
    ethnoi = load('ethnos')
    recipes = load('recipes')
    buildings = load('buildings')
    knowledge = {k['name']: label(k) for k in load('knowledge')}
    crops = {c['name']: label(c) for c in load('crops')}
    animals = {a['name']: label(a) for a in load('animals')}
    items = {i['name']: label(i) for i in load('items')}

    def open_to(entries, known):
        """What a culture can actually do: an entry with no knowledge listed is
        open to everybody, one with a method is open only to those who have it."""
        yes, no = [], []
        for e in entries:
            need = e.get('knowledge')
            (yes if (not need or need in known) else no).append(e)
        return yes, no

    lines = []
    lines.append('# Этносы и контент')
    lines.append('')
    lines.append('**Файл собирается автоматически:** `python3 tools/ethnos_table.py`.')
    lines.append('Правится не он, а `content/` — иначе таблица разойдётся с игрой (D100).')
    lines.append('')
    lines.append(f'Народов: {len(ethnoi)}. Рецептов: {len(recipes)}. Строений: {len(buildings)}. '
                 f'Методов: {len(knowledge)}.')
    lines.append('')

    # --- the summary table --------------------------------------------------
    lines.append('## Сводка')
    lines.append('')
    lines.append('| Народ | Страна | Сеет | Пасёт | Знает методов | Открыто рецептов | Открыто строений |')
    lines.append('| --- | --- | --- | --- | --- | --- | --- |')
    for e in sorted(ethnoi, key=lambda x: x['name']):
        known = set(e.get('common_knowledge', [])) | set(e.get('family_knowledge_pool', []))
        r_yes, _ = open_to(recipes, known)
        b_yes, _ = open_to(buildings, known)
        sown = ', '.join(crops.get(c, c) for c in e.get('crops', [])) or '—'
        herd = ', '.join(f'{animals.get(a, a)} x {n}'
                         for a, n in (e.get('starting_livestock') or {}).items()) or '—'
        lines.append(f"| **{label(e)}** (`{e['name']}`) | {e.get('biome', 'temperate')} | {sown} | "
                     f"{herd} | {len(known)} | {len(r_yes)}/{len(recipes)} | {len(b_yes)}/{len(buildings)} |")
    lines.append('')

    # --- what only one of them has -----------------------------------------
    lines.append('## Чем они отличаются')
    lines.append('')
    lines.append('Методы, которые есть не у всех: это и есть разница между народами.')
    lines.append('')
    everyone = set.intersection(*[set(e.get('common_knowledge', [])) for e in ethnoi]) if ethnoi else set()
    lines.append('| Метод | ' + ' | '.join(label(e) for e in sorted(ethnoi, key=lambda x: x['name'])) + ' |')
    lines.append('| --- | ' + ' | '.join('---' for _ in ethnoi) + ' |')
    for method in sorted(knowledge):
        if method in everyone:
            continue
        row = []
        for e in sorted(ethnoi, key=lambda x: x['name']):
            if method in e.get('common_knowledge', []):
                row.append('да')
            elif method in e.get('family_knowledge_pool', []):
                row.append('семья')
            else:
                row.append('—')
        if set(row) == {'—'}:
            continue
        lines.append(f'| {method} — {knowledge[method]} | ' + ' | '.join(row) + ' |')
    lines.append('')
    lines.append('«семья» — метод не общий, его держит одна семья из пула '
                 '(`family_knowledge_pool`), и с её смертью он теряется.')
    lines.append('')

    # --- one section per people --------------------------------------------
    for e in sorted(ethnoi, key=lambda x: x['name']):
        known = set(e.get('common_knowledge', [])) | set(e.get('family_knowledge_pool', []))
        r_yes, r_no = open_to(recipes, known)
        b_yes, b_no = open_to(buildings, known)

        lines.append(f"## {label(e)} (`{e['name']}`)")
        lines.append('')
        comment = e.get('_comment')
        if comment:
            lines.append(f'> {comment}')
            lines.append('')
        lines.append(f"- **Страна:** {e.get('biome', 'temperate')}")
        lines.append(f"- **Язык:** {e.get('language', '—')}, **постройки:** {e.get('architecture', '—')}")
        lines.append(f"- **Сеет:** {', '.join(crops.get(c, c) for c in e.get('crops', [])) or '—'}")
        herd = (e.get('starting_livestock') or {})
        lines.append(f"- **Приходит со скотом:** "
                     f"{', '.join(f'{animals.get(a, a)} x {n}' for a, n in herd.items()) or '—'}")
        lines.append(f"- **Ремёсла:** {', '.join(e.get('trades', [])) or '—'}")
        lines.append(f"- **Любимая еда:** "
                     f"{', '.join(items.get(f, f) for f in e.get('preferred_foods', [])) or '—'}")
        lines.append(f"- **Умения:** база {e.get('base_skill', 0)}, у мастера "
                     f"{e.get('specialist_skill', 0)}")
        lines.append('')
        lines.append(f"**Знает ({len(e.get('common_knowledge', []))}):** "
                     + ', '.join(sorted(e.get('common_knowledge', []))))
        pool = e.get('family_knowledge_pool', [])
        if pool:
            lines.append('')
            lines.append(f"**Семейное знание ({e.get('family_knowledge_draws', 0)} из пула):** "
                         + ', '.join(sorted(pool)))
        lines.append('')

        # What this culture cannot do, which is the interesting half.
        shut_r = sorted({r.get('knowledge') for r in r_no if r.get('knowledge')})
        shut_b = sorted({label(b) for b in b_no})
        lines.append(f'**Открыто:** рецептов {len(r_yes)} из {len(recipes)}, '
                     f'строений {len(b_yes)} из {len(buildings)}.')
        if shut_b:
            lines.append('')
            lines.append('**Не построит:** ' + ', '.join(shut_b))
        if shut_r:
            lines.append('')
            lines.append('**Не умеет (методы):** ' + ', '.join(shut_r))
        lines.append('')

    out = ROOT / 'doc' / 'ETHNOS_CONTENT.md'
    out.write_text('\n'.join(lines) + '\n')
    print(f'wrote {out.relative_to(ROOT)}: {len(ethnoi)} peoples')


if __name__ == '__main__':
    main()

