
#!/usr/bin/env python3
"""
build_csv.py — Phase A: GeoNames txt → filtered CSV (all countries)
====================================================================
Reads 6 GeoNames source files, ALL countries, writes 6 clean CSVs
ready for LMDB indexing.

Input files required in --data-dir:
  admin1CodesASCII.txt     — from download.geonames.org/export/dump/
  admin2Codes.txt          — from download.geonames.org/export/dump/
  postal_codes.txt         — from download.geonames.org/export/zip/allCountries.zip
  cities15000.txt          — from download.geonames.org/export/dump/cities15000.zip
  allCountries.txt         — from download.geonames.org/export/dump/allCountries.zip
  iso3166.csv              — from github.com/lukes/ISO-3166-Countries-with-Regional-Codes
  alternateNamesV2.txt     — from download.geonames.org/export/dump/alternateNamesV2.zip

Output files (in --out-dir):
  admin1_filtered.csv       — CC:admin1_code → state name       (3,862 rows, all countries)
  admin2_filtered.csv       — CC:adm1:adm2   → district name    (47,496 rows, all countries)
  postal_filtered.csv       — CC:postal_code → city + state     (1,080,414 rows, all countries)
  cities_filtered.csv       — CC:city_name   → state name       (3,220,878 rows, all countries)
  countries_filtered.csv    — alpha2/alpha3/numeric/name → canonical (249 countries)
  city_aliases_filtered.csv — CC:alias → canonical city name    (2,661 English aliases)

Why CSV is much smaller than source txt:
  postal_codes.txt (135MB) → postal_filtered.csv (~30MB)
    Source: ~1M rows × 12 cols (lat/lon, accuracy, admin2-4 etc)
    Output: ~1M rows × 5 cols (country, postal, city, state, admin1)
    All countries kept — size reduction from dropping unused columns only

  allCountries.txt (1.7GB) → cities_filtered.csv (~120MB)
    Source: 13.4M rows ALL feature types (mountains, rivers, lakes, parks, roads etc)
    Output: populated places only (feature_class = P) — ~3.2M rows
    All countries kept — size reduction from feature filtering only

  alternateNamesV2.txt (750MB) → city_aliases_filtered.csv (~100KB)
    Source: 18.7M rows ALL languages (zh, ar, ru, link, wkdt etc)
    Output: English names (lang=en) for cities in cities15000 only — 2,661 rows

Actual output row counts:
  admin1_filtered.csv       — 3,863 rows  (3,862 data + 1 header)
  admin2_filtered.csv       — 47,497 rows (47,496 data + 1 header)
  postal_filtered.csv       — 1,080,415 rows
  cities_filtered.csv       — 3,220,879 rows
  countries_filtered.csv    — 250 rows (249 + header)
  city_aliases_filtered.csv — 2,662 rows

Usage:
  python3 tools/build_csv.py \\
      --data-dir ~/libpostal_data/geonames \\
      --out-dir  ~/libpostal_data/geonames/csv
"""


import csv
import os
import sys
import argparse
import time

def normalize(s: str) -> str:
    return s.strip().lower()

def log(msg: str):
    print(f"[build_csv] {msg}", flush=True)


# =============================================================================
#  Step 1 — admin1CodesASCII.txt → admin1_filtered.csv (ALL countries)
#  Output: country_code, admin1_code, state_name
# =============================================================================
def build_admin1(src: str, dst: str) -> dict:
    log(f"Building admin1: {src} → {dst}")
    admin1_map = {}  # "IN:19" → "karnataka"
    count = 0
    with open(src, encoding="utf-8") as f_in, \
         open(dst, "w", newline="", encoding="utf-8") as f_out:
        writer = csv.writer(f_out)
        writer.writerow(["country_code", "admin1_code", "state_name"])
        for line in f_in:
            line = line.rstrip("\n")
            if not line: continue
            cols = line.split("\t")
            if len(cols) < 2: continue
            dot = cols[0].find(".")
            if dot < 0: continue
            cc   = cols[0][:dot]
            adm1 = cols[0][dot+1:]
            state = normalize(cols[1])
            if not state: continue
            writer.writerow([cc, adm1, state])
            admin1_map[f"{cc}:{adm1}"] = state
            count += 1
    log(f"  admin1 rows: {count}")
    return admin1_map


# =============================================================================
#  Step 2 — admin2Codes.txt → admin2_filtered.csv (ALL countries)
#  Format: CC.adm1.adm2 \t name \t ascii_name \t geonameid
#  Output: country_code, admin1_code, admin2_code, district_name
# =============================================================================
def build_admin2(src: str, dst: str) -> int:
    log(f"Building admin2: {src} → {dst}")
    count = 0
    with open(src, encoding="utf-8") as f_in, \
         open(dst, "w", newline="", encoding="utf-8") as f_out:
        writer = csv.writer(f_out)
        writer.writerow(["country_code", "admin1_code", "admin2_code", "district_name"])
        for line in f_in:
            line = line.rstrip("\n")
            if not line: continue
            cols = line.split("\t")
            if len(cols) < 2: continue
            parts = cols[0].split(".")
            if len(parts) < 3: continue
            cc, adm1, adm2 = parts[0], parts[1], parts[2]
            name = normalize(cols[1])
            if not name: continue
            writer.writerow([cc, adm1, adm2, name])
            count += 1
    log(f"  admin2 rows: {count}")
    return count


# =============================================================================
#  Step 3 — postal_codes.txt → postal_filtered.csv (ALL countries)
#  Output: country_code, postal_code, city, state, admin1_code
# =============================================================================
def build_postal(src: str, dst: str) -> int:
    log(f"Building postal: {src} → {dst}")
    seen = set()
    count = 0
    with open(src, encoding="utf-8") as f_in, \
         open(dst, "w", newline="", encoding="utf-8") as f_out:
        writer = csv.writer(f_out)
        writer.writerow(["country_code", "postal_code", "city", "state", "admin1_code"])
        for line in f_in:
            line = line.rstrip("\n")
            if not line: continue
            cols = line.split("\t")
            if len(cols) < 5: continue
            cc     = cols[0].strip()
            postal = cols[1].strip()
            city   = normalize(cols[2])
            state  = normalize(cols[3])
            adm1   = cols[4].strip()
            if not cc or not postal: continue
            # GB: keep outward code only
            if cc == "GB":
                p = postal.upper()
                postal = p.split()[0] if " " in p else p
            else:
                postal = normalize(postal)
            key = f"{cc}:{postal}"
            if key in seen: continue
            seen.add(key)
            writer.writerow([cc, postal, city, state, adm1])
            count += 1
    log(f"  postal rows: {count}")
    return count


# =============================================================================
#  Step 4 — cities15000.txt + allCountries.txt → cities_filtered.csv
#  Output: country_code, city_name, state_name, admin1_code, source
# =============================================================================
def build_cities(cities15k_src: str, all_countries_src: str,
                 admin1_map: dict, dst: str) -> int:
    log(f"Building cities → {dst}")
    seen = set()
    count_15k = count_all = 0

    with open(dst, "w", newline="", encoding="utf-8") as f_out:
        writer = csv.writer(f_out)
        writer.writerow(["country_code", "city_name", "state_name",
                         "admin1_code", "source"])

        # Pass 1: cities15000 (higher quality)
        log("  Pass 1: cities15000.txt ...")
        with open(cities15k_src, encoding="utf-8") as f_in:
            for line in f_in:
                line = line.rstrip("\n")
                if not line: continue
                cols = line.split("\t")
                if len(cols) < 11: continue
                cc   = cols[8].strip()
                feat = cols[7].strip()
                if not feat.startswith("P"): continue
                city  = normalize(cols[1])
                adm1  = cols[10].strip()
                state = admin1_map.get(f"{cc}:{adm1}", "")
                if not city or not state: continue
                key = f"{cc}:{city}"
                if key in seen: continue
                seen.add(key)
                writer.writerow([cc, city, state, adm1, "cities15000"])
                count_15k += 1
        log(f"  cities15000 rows: {count_15k}")

        # Pass 2: allCountries (fills remaining)
        log("  Pass 2: allCountries.txt (13M rows)...")
        t = time.time()
        with open(all_countries_src, encoding="utf-8") as f_in:
            for i, line in enumerate(f_in):
                if i % 1_000_000 == 0 and i > 0:
                    log(f"    ...{i//1_000_000}M rows ({time.time()-t:.1f}s, new={count_all})")
                line = line.rstrip("\n")
                if not line: continue
                cols = line.split("\t")
                if len(cols) < 11: continue
                cc         = cols[8].strip()
                feat_class = cols[6].strip()
                if feat_class != "P": continue
                city  = normalize(cols[1])
                adm1  = cols[10].strip()
                state = admin1_map.get(f"{cc}:{adm1}", "")
                if not city or not state: continue
                key = f"{cc}:{city}"
                if key in seen: continue
                seen.add(key)
                writer.writerow([cc, city, state, adm1, "allCountries"])
                count_all += 1
        log(f"  allCountries new rows: {count_all}")

    total = count_15k + count_all
    log(f"  cities total: {total}")
    return total


# =============================================================================
#  Step 5 — iso3166.csv → countries_filtered.csv (ALL countries)
#  Input CSV columns: name,alpha-2,alpha-3,country-code,iso_3166-2,...
#  Output: alpha2, alpha3, numeric_code, country_name, region, sub_region
# =============================================================================
def build_countries(src: str, dst: str) -> dict:
    log(f"Building countries: {src} → {dst}")
    # Returns map: "IN" → "india", "IND" → "india", "356" → "india" etc
    country_map = {}
    count = 0
    with open(src, encoding="utf-8") as f_in, \
         open(dst, "w", newline="", encoding="utf-8") as f_out:
        reader = csv.DictReader(f_in)
        writer = csv.writer(f_out)
        writer.writerow(["alpha2", "alpha3", "numeric_code",
                         "country_name", "region", "sub_region"])
        for row in reader:
            name    = normalize(row.get("name", ""))
            alpha2  = row.get("alpha-2", "").strip().upper()
            alpha3  = row.get("alpha-3", "").strip().upper()
            numeric = row.get("country-code", "").strip()
            region  = normalize(row.get("region", ""))
            subreg  = normalize(row.get("sub-region", ""))
            if not alpha2 or not name: continue
            writer.writerow([alpha2, alpha3, numeric, name, region, subreg])
            # Build reverse lookup map
            country_map[alpha2]  = name
            country_map[alpha3]  = name
            if numeric: country_map[numeric] = name
            count += 1
    log(f"  countries rows: {count}")
    return country_map


# =============================================================================
#  Step 6 — alternateNamesV2.txt → city_aliases_filtered.csv
#
#  We need a map from geonameid → canonical city name first (from cities file)
#  Then: alternate English name → canonical name
#
#  Input cols: alternateNameId, geonameid, isolanguage, alternateName,
#              isPreferredName, isShortName, isColloquial, isHistoric, ...
#
#  We keep: isolanguage = "en" (English names only)
#  Output:  alias_lower, canonical_name, country_code, geonameid
# =============================================================================
def build_city_aliases(alt_names_src: str, cities15k_src: str, dst: str) -> int:
    log(f"Building city aliases → {dst}")

    # Step A: build geonameid → (canonical_name, country_code) from cities15000
    log("  Loading cities15000 geoname IDs...")
    geoname_map = {}  # geonameid → (name, cc)
    with open(cities15k_src, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line: continue
            cols = line.split("\t")
            if len(cols) < 9: continue
            gid  = cols[0].strip()
            name = normalize(cols[1])
            cc   = cols[8].strip()
            feat = cols[7].strip()
            if feat.startswith("P") and name and cc:
                geoname_map[gid] = (name, cc)
    log(f"  Loaded {len(geoname_map)} city geoname IDs")

    # Step B: scan alternateNamesV2 for English names
    log("  Scanning alternateNamesV2.txt for English aliases (18M rows)...")
    t = time.time()
    count = 0
    seen_aliases = set()

    with open(alt_names_src, encoding="utf-8") as f_in, \
         open(dst, "w", newline="", encoding="utf-8") as f_out:
        writer = csv.writer(f_out)
        writer.writerow(["alias_lower", "canonical_name", "country_code", "geonameid"])

        for i, line in enumerate(f_in):
            if i % 2_000_000 == 0 and i > 0:
                log(f"    ...{i//1_000_000}M rows ({time.time()-t:.1f}s, aliases={count})")
            line = line.rstrip("\n")
            if not line: continue
            cols = line.split("\t")
            if len(cols) < 4: continue

            gid      = cols[1].strip()
            lang     = cols[2].strip()
            alt_name = cols[3].strip()

            # Only English names for cities we know about
            if lang != "en": continue
            if gid not in geoname_map: continue
            if not alt_name: continue

            canonical, cc = geoname_map[gid]
            alias = normalize(alt_name)

            # Skip if alias == canonical (no value)
            if alias == canonical: continue

            key = f"{cc}:{alias}"
            if key in seen_aliases: continue
            seen_aliases.add(key)

            writer.writerow([alias, canonical, cc, gid])
            count += 1

    log(f"  city aliases written: {count}")
    return count


# =============================================================================
#  Main
# =============================================================================
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--data-dir", required=True)
    parser.add_argument("--out-dir",  required=True)
    args = parser.parse_args()

    data_dir = os.path.expanduser(args.data_dir)
    out_dir  = os.path.expanduser(args.out_dir)
    os.makedirs(out_dir, exist_ok=True)

    t_start = time.time()
    log(f"Source dir : {data_dir}")
    log(f"Output dir : {out_dir}")
    log(f"Countries  : ALL")
    print()

    # Step 1: admin1
    admin1_map = build_admin1(
        src=os.path.join(data_dir, "admin1CodesASCII.txt"),
        dst=os.path.join(out_dir,  "admin1_filtered.csv"))
    print()

    # Step 2: admin2
    build_admin2(
        src=os.path.join(data_dir, "admin2Codes.txt"),
        dst=os.path.join(out_dir,  "admin2_filtered.csv"))
    print()

    # Step 3: postal (all countries)
    build_postal(
        src=os.path.join(data_dir, "postal_codes.txt"),
        dst=os.path.join(out_dir,  "postal_filtered.csv"))
    print()

    # Step 4: cities (all countries)
    build_cities(
        cities15k_src    =os.path.join(data_dir, "cities15000.txt"),
        all_countries_src=os.path.join(data_dir, "allCountries.txt"),
        admin1_map       =admin1_map,
        dst              =os.path.join(out_dir,  "cities_filtered.csv"))
    print()

    # Step 5: countries ISO 3166
    build_countries(
        src=os.path.join(data_dir, "iso3166.csv"),
        dst=os.path.join(out_dir,  "countries_filtered.csv"))
    print()

    # Step 6: city aliases (English alternates from alternateNamesV2)
    build_city_aliases(
        alt_names_src=os.path.join(data_dir, "alternateNamesV2.txt"),
        cities15k_src=os.path.join(data_dir, "cities15000.txt"),
        dst          =os.path.join(out_dir,  "city_aliases_filtered.csv"))
    print()

    elapsed = time.time() - t_start
    log(f"Done in {elapsed:.1f}s")
    log(f"CSV files in: {out_dir}")
    log("Next: run build_lmdb to index all CSVs")


if __name__ == "__main__":
    main()