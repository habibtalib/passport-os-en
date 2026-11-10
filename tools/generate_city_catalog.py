#!/usr/bin/env python3
"""Build an offline, gzip city directory. No device heap is used to serve it.

Coordinates are weather-location coordinates from the MIT-licensed snapshot,
not a claim that this is a current administrative-division register.
"""
import argparse
import csv
import gzip
import hashlib
import json
import io
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# English province names (same spelling as kProvinces in main/apps/radio/radio_catalog.cc).
# The Chinese keys are source data and must stay Chinese: they are matched against the snapshot.
PROVINCE_EN = {
    '北京': 'Beijing', '天津': 'Tianjin', '河北': 'Hebei', '山西': 'Shanxi',
    '内蒙古': 'Inner Mongolia', '辽宁': 'Liaoning', '吉林': 'Jilin',
    '黑龙江': 'Heilongjiang', '上海': 'Shanghai', '江苏': 'Jiangsu',
    '浙江': 'Zhejiang', '安徽': 'Anhui', '福建': 'Fujian', '江西': 'Jiangxi',
    '山东': 'Shandong', '河南': 'Henan', '湖北': 'Hubei', '湖南': 'Hunan',
    '广东': 'Guangdong', '广西': 'Guangxi', '海南': 'Hainan', '重庆': 'Chongqing',
    '四川': 'Sichuan', '贵州': 'Guizhou', '云南': 'Yunnan', '西藏': 'Tibet',
    '陕西': 'Shaanxi', '甘肃': 'Gansu', '青海': 'Qinghai', '宁夏': 'Ningxia',
    '新疆': 'Xinjiang', '香港': 'Hong Kong', '澳门': 'Macau', '台湾': 'Taiwan',
}
# Display-name overrides where the snapshot pinyin is not the common English name.
CITY_EN = {
    '香港': 'Hong Kong', '澳门': 'Macau', '台北': 'Taipei', '高雄': 'Kaohsiung',
    '广州': 'Guangzhou', '呼和浩特': 'Hohhot', '哈尔滨': 'Harbin',
    '乌鲁木齐': 'Urumqi', '拉萨': 'Lhasa', '重庆': 'Chongqing',
    '杨陵': 'Yangling District', '云安': "Yun'an",
}
SOURCE = ROOT / 'third_party/china-cities/china_cities.txt'
SNAPSHOT_SHA256 = '8171af06c2f2c9a44b36565e51e25eed05828137d70807d2828763146abddba3'


def generate():
    raw = SOURCE.read_bytes()
    assert hashlib.sha256(raw).hexdigest() == SNAPSHOT_SHA256, 'city source snapshot changed'
    # Preserve the previous release's urban-centre coordinates and short names.
    legacy = json.loads((ROOT / 'third_party/china-cities/legacy_centres.json').read_text('utf-8'))
    rows = []
    display = []
    translations = set()
    names = set()
    for r in csv.reader(raw.decode('utf-8').splitlines()[1:], delimiter='\t'):
        if len(r) != 12:
            raise ValueError(f'bad row: {r}')
        code, english, name, _, _, _, _, province, _, parent, lat, lon = r
        if name == parent:
            translations.add((english, name, province))
        latitude, longitude = round(float(lat), 4), round(float(lon), 4)
        if not (3 <= latitude <= 55 and 70 <= longitude <= 140):
            raise ValueError(f'coordinate out of range: {code}')
        if name == '开县':
            continue  # modern 开州 record is already present in the same snapshot
        if province in ('北京', '天津', '上海', '重庆'):
            parent = province
        if province == '山东' and parent == '莱芜':
            parent = '济南'
        if name == '莱芜':
            parent = '济南'
        key = (province, name, parent)
        if key in names:
            continue
        names.add(key)
        if name in legacy:
            latitude, longitude = legacy[name]
        country = {'香港': 'HK', '澳门': 'MO', '台湾': 'TW'}.get(province, 'CN')
        # id, display name, latitude, longitude, province, parent city, radio country
        # (shipped rows add column 7: the parent city's English name)
        rows.append([code, name, latitude, longitude, province, parent, country])
        # Shipped row: English display name and province; the parent city stays Chinese
        # because it is the radio-directory search key (profile.radio_city).
        display.append([code, CITY_EN.get(name, english.capitalize()), latitude, longitude,
                        PROVINCE_EN[province], parent, country])
    # Column 7: English name of the parent city, so the phone page can search districts
    # by their parent in English (e.g. "Zhuhai" also finds Doumen and Jinwan).
    parent_en = {r[1]: d[1] for r, d in zip(rows, display) if r[1] == r[5]}
    for r, d in zip(rows, display):
        d.append(parent_en.get(r[5], d[1] if r[1] == r[5] else ''))
    # Distinct Chinese places can share one pinyin name in the same province
    # (e.g. three "Qiaoxi" districts in Hebei). Append the parent city to tell them apart.
    groups = {}
    for d in display:
        groups.setdefault((d[4], d[1]), []).append(d)
    for same in groups.values():
        if len(same) > 1:
            for d in same:
                if d[7] and d[7] != d[1]:
                    d[1] = f'{d[1]} ({d[7]})'
    seen = {}
    for d in display:
        seen.setdefault((d[4], d[1]), []).append(d)
    for same in seen.values():
        if len(same) > 1:
            for i, d in enumerate(same[1:], 2):
                d[1] = f'{d[1]} {i}'
    assert len({(d[4], d[1]) for d in display}) == len(display), 'ambiguous city display names'
    assert len(rows) >= 3000 and len({r[4] for r in rows}) == 34
    assert any(r[1] == '珠海' and r[4] == '广东' for r in rows)
    assert len(display) == len(rows)
    payload = json.dumps(display, ensure_ascii=False, separators=(',', ':')).encode('utf-8')
    compressed = io.BytesIO()
    with gzip.GzipFile(filename='', fileobj=compressed, mode='wb', compresslevel=9, mtime=0) as writer:
        writer.write(payload)
    symbols = ''.join(sorted(set(''.join(r[1] + r[4] + r[5] for r in rows)))) + '\n'
    outputs = {
        ROOT / 'main/apps/home/cities.json': payload + b'\n',
        ROOT / 'main/apps/home/cities.json.gz': compressed.getvalue(),
        ROOT / 'main/fonts/city_symbols.txt': symbols.encode('utf-8'),
        ROOT / 'main/apps/radio/radio_cities.inc': (
            '// Generated by tools/generate_city_catalog.py; MIT source in third_party.\n'
            + ''.join(f'{{"{e}", "{n}", "{p}"}},\n' for e, n, p in sorted(translations))
        ).encode('utf-8'),
    }
    return display, outputs


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    rows, outputs = generate()
    for path, value in outputs.items():
        if args.check:
            assert path.read_bytes() == value, f'regenerate {path.relative_to(ROOT)}'
        else:
            path.write_bytes(value)
    print(f'City catalog: {len(rows)} locations, 34 provinces, '
          f'{len(outputs[ROOT / "main/apps/home/cities.json.gz"])} bytes in flash')
