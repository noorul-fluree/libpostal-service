// =============================================================================
//  build_lmdb.cc — Phase B: CSV → LMDB index builder (all countries)
//
//  Builds 7 LMDB databases from filtered CSV files:
//
//    admin1.lmdb         key="IN:19"           value="karnataka"
//    admin2.lmdb         key="IN:19:1234"      value="bengaluru urban"
//    postal.lmdb         key="IN:560001"       value="bengaluru|karnataka"
//    postal_reverse.lmdb key="IN:bengaluru"    value="560001,560002,..."
//    cities.lmdb         key="IN:bengaluru"    value="karnataka"
//    countries.lmdb      key="IN"              value="india|IN|IND|356|asia|southern asia"
//    aliases.lmdb        key="IN:bombay"       value="mumbai"
//
//  Build:
//    g++ -O2 -std=c++17 tools/build_lmdb.cc -o tools/build_lmdb -llmdb
//
//  Run:
//    ./tools/build_lmdb \
//        --csv-dir  ~/libpostal_data/geonames/csv \
//        --lmdb-dir ~/libpostal_data/geonames/lmdb
// =============================================================================

#include <lmdb.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>
#include <filesystem>
#include <chrono>
#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;

static void check(int rc, const char* ctx) {
    if (rc != MDB_SUCCESS)
        throw std::runtime_error(std::string(ctx) + ": " + mdb_strerror(rc));
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::vector<std::string> parseCsvLine(const std::string& line) {
    std::vector<std::string> cols;
    std::string cur;
    bool in_quotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"') {
            if (in_quotes && i+1 < line.size() && line[i+1] == '"') {
                cur += '"'; ++i;
            } else { in_quotes = !in_quotes; }
        } else if (c == ',' && !in_quotes) {
            cols.push_back(trim(cur)); cur.clear();
        } else { cur += c; }
    }
    cols.push_back(trim(cur));
    return cols;
}

// =============================================================================
//  LmdbWriter
// =============================================================================
class LmdbWriter {
public:
    LmdbWriter(const std::string& path, size_t map_size_mb) {
        fs::create_directories(path);
        check(mdb_env_create(&env_), "mdb_env_create");
        check(mdb_env_set_mapsize(env_,
            static_cast<size_t>(map_size_mb) * 1024 * 1024), "set_mapsize");
        check(mdb_env_open(env_, path.c_str(), MDB_NOSYNC, 0664), "env_open");
        check(mdb_txn_begin(env_, nullptr, 0, &txn_), "txn_begin");
        check(mdb_dbi_open(txn_, nullptr, MDB_CREATE, &dbi_), "dbi_open");
        path_ = path;
    }

    void put(const std::string& key, const std::string& val) {
        MDB_val k, v;
        k.mv_data = const_cast<char*>(key.data()); k.mv_size = key.size();
        v.mv_data = const_cast<char*>(val.data());  v.mv_size = val.size();
        int rc = mdb_put(txn_, dbi_, &k, &v, MDB_NOOVERWRITE);
        if (rc == MDB_KEYEXIST) return;
        check(rc, "mdb_put");
        if (++count_ % 200000 == 0) {
            check(mdb_txn_commit(txn_), "commit batch");
            check(mdb_txn_begin(env_, nullptr, 0, &txn_), "txn_begin batch");
        }
    }

    void commit() {
        check(mdb_txn_commit(txn_), "commit final");
        txn_ = nullptr;
        mdb_dbi_close(env_, dbi_);
        mdb_env_sync(env_, 1);
        mdb_env_close(env_);
        env_ = nullptr;
        std::cout << "[build_lmdb]   " << path_
                  << " → " << count_ << " entries\n";
    }

    ~LmdbWriter() {
        if (txn_) mdb_txn_abort(txn_);
        if (env_) mdb_env_close(env_);
    }

private:
    MDB_env* env_ = nullptr;
    MDB_txn* txn_ = nullptr;
    MDB_dbi  dbi_ = 0;
    size_t   count_ = 0;
    std::string path_;
};

// =============================================================================
//  build_admin1
// =============================================================================
static void build_admin1(const std::string& csv, const std::string& lmdb_dir) {
    std::cout << "[build_lmdb] Building admin1.lmdb...\n";
    LmdbWriter db(lmdb_dir + "/admin1.lmdb", 32);
    std::ifstream f(csv);
    if (!f.is_open()) throw std::runtime_error("Cannot open: " + csv);
    std::string line; std::getline(f, line);
    while (std::getline(f, line)) {
        auto c = parseCsvLine(line);
        if (c.size() < 3 || c[0].empty() || c[1].empty() || c[2].empty()) continue;
        db.put(c[0] + ":" + c[1], c[2]);
    }
    db.commit();
}

// =============================================================================
//  build_admin2
// =============================================================================
static void build_admin2(const std::string& csv, const std::string& lmdb_dir) {
    std::cout << "[build_lmdb] Building admin2.lmdb...\n";
    LmdbWriter db(lmdb_dir + "/admin2.lmdb", 64);
    std::ifstream f(csv);
    if (!f.is_open()) throw std::runtime_error("Cannot open: " + csv);
    std::string line; std::getline(f, line);
    while (std::getline(f, line)) {
        auto c = parseCsvLine(line);
        if (c.size() < 4 || c[0].empty() || c[3].empty()) continue;
        db.put(c[0] + ":" + c[1] + ":" + c[2], c[3]);
    }
    db.commit();
}

// =============================================================================
//  build_postal
// =============================================================================
static void build_postal(const std::string& csv, const std::string& lmdb_dir) {
    std::cout << "[build_lmdb] Building postal.lmdb (1M entries)...\n";
    LmdbWriter db(lmdb_dir + "/postal.lmdb", 256);
    std::ifstream f(csv);
    if (!f.is_open()) throw std::runtime_error("Cannot open: " + csv);
    std::string line; std::getline(f, line);
    while (std::getline(f, line)) {
        auto c = parseCsvLine(line);
        if (c.size() < 4 || c[0].empty() || c[1].empty()) continue;
        db.put(c[0] + ":" + c[1], c[2] + "|" + c[3]);
    }
    db.commit();
}

// =============================================================================
//  build_postal_reverse
//
//  Three strategies combined:
//
//  Strategy A (all countries): postal CSV city column IS the city name
//    e.g. US: "akutan", GB: "manchester" → direct key
//
//  Strategy B (India parentheses): post office name contains city in ()
//    e.g. "bharat nagar (mumbai)" → extract "mumbai"
//
//  Strategy C (India substring): post office name starts with known city
//    e.g. "bangalore g.p.o." → prefix "bangalore" matches known city
//
//  Key: "IN:bengaluru"  Value: "560001,560002,..."  (max 50)
//  Key: "IN:karnataka"  Value: state-level codes    (max 50)
// =============================================================================
static void build_postal_reverse(const std::string& postal_csv,
                                  const std::string& cities_csv,
                                  const std::string& lmdb_dir) {
    std::cout << "[build_lmdb] Building postal_reverse.lmdb...\n";

    static const int CAP = 50;

    auto extractParenCity = [](const std::string& s) -> std::string {
        auto lp = s.rfind('(');
        auto rp = s.rfind(')');
        if (lp != std::string::npos && rp != std::string::npos && rp > lp)
            return s.substr(lp + 1, rp - lp - 1);
        return {};
    };

    // Load aliases map: "IN:bangalore" → "bengaluru" etc
    std::unordered_map<std::string, std::string> alias_map;

    // Load aliases: key="IN:bangalore" value="bengaluru"
    {
        std::string aliases_csv = cities_csv.substr(0, cities_csv.rfind('/'))
                                  + "/city_aliases_filtered.csv";
        std::ifstream fa(aliases_csv);
        if (fa.is_open()) {
            std::string line; std::getline(fa, line);
            while (std::getline(fa, line)) {
                auto c = parseCsvLine(line);
                // cols: alias_lower, canonical_name, country_code, geonameid
                if (c.size() < 3 || c[0].empty() || c[1].empty() || c[2].empty()) continue;
                alias_map[c[2] + ":" + c[0]] = c[1]; // "IN:bangalore" → "bengaluru"
            }
        }
    }

    // Load known cities set for Strategy A + C matching
    std::unordered_set<std::string> known_cities;
    {
        std::ifstream f(cities_csv);
        if (!f.is_open()) throw std::runtime_error("Cannot open: " + cities_csv);
        std::string line; std::getline(f, line);
        while (std::getline(f, line)) {
            auto c = parseCsvLine(line);
            if (c.size() < 2 || c[0].empty() || c[1].empty()) continue;
            known_cities.insert(c[0] + ":" + c[1]);
        }
    }

    std::unordered_map<std::string, std::vector<std::string>> admin_map;
    std::unordered_map<std::string, std::vector<std::string>> state_map;
    std::unordered_map<std::string, std::vector<std::string>> direct_city_map;

    {
        std::ifstream f(postal_csv);
        if (!f.is_open()) throw std::runtime_error("Cannot open: " + postal_csv);
        std::string line; std::getline(f, line);
        while (std::getline(f, line)) {
            auto c = parseCsvLine(line);
            if (c.size() < 5 || c[0].empty() || c[1].empty()) continue;
            const std::string& cc    = c[0];
            const std::string& pc    = c[1];
            const std::string& pname = c[2];
            const std::string& state = c[3];
            const std::string& adm1  = c[4];

            if (!adm1.empty()) {
                auto& v = admin_map[cc + ":" + adm1];
                if ((int)v.size() < CAP) v.push_back(pc);
            }
            if (!state.empty()) {
                auto& v = state_map[cc + ":" + state];
                if ((int)v.size() < CAP) v.push_back(pc);
            }

            // Strategy A: place name is a known city (US/GB style)
            if (!pname.empty() && known_cities.count(cc + ":" + pname)) {
                auto& v = direct_city_map[cc + ":" + pname];
                if ((int)v.size() < CAP) v.push_back(pc);
            }

            // Strategy B: extract city from parentheses
            if (!pname.empty()) {
                std::string paren = extractParenCity(pname);
                if (!paren.empty()) {
                    auto& v = direct_city_map[cc + ":" + paren];
                    if ((int)v.size() < CAP) v.push_back(pc);
                }
            }

            // Strategy C: India prefix match — with alias resolution
            // "bangalore g.p.o." → prefix "bangalore" → alias → "bengaluru"
            if (cc == "IN" && !pname.empty()) {
                for (size_t i = 1; i < pname.size(); ++i) {
                    if (pname[i] == ' ') {
                        std::string prefix = pname.substr(0, i);
                        std::string lookup_key = cc + ":" + prefix;
                        // Try direct match first
                        std::string city_key = lookup_key;
                        if (!known_cities.count(city_key)) {
                            // Try alias resolution: bangalore → bengaluru
                            auto ait = alias_map.find(lookup_key);
                            if (ait != alias_map.end())
                                city_key = cc + ":" + ait->second;
                        }
                        if (known_cities.count(city_key)) {
                            auto& v = direct_city_map[city_key];
                            if ((int)v.size() < CAP) v.push_back(pc);
                            break;
                        }
                    }
                }
            }
        }
    }

    std::cout << "[build_lmdb]   admin1 buckets: " << admin_map.size()
              << "  state buckets: " << state_map.size()
              << "  direct city entries: " << direct_city_map.size() << "\n";

    LmdbWriter db(lmdb_dir + "/postal_reverse.lmdb", 2048);

    // State-level entries
    for (const auto& [key, pcs] : state_map) {
        if (pcs.empty()) continue;
        std::string val;
        for (const auto& pc : pcs) { if (!val.empty()) val += ','; val += pc; }
        db.put(key, val);
    }

    // City-level from cities CSV
    {
        std::ifstream f(cities_csv);
        if (!f.is_open()) throw std::runtime_error("Cannot open: " + cities_csv);
        std::string line; std::getline(f, line);
        while (std::getline(f, line)) {
            auto c = parseCsvLine(line);
            if (c.size() < 4 || c[0].empty() || c[1].empty() || c[3].empty()) continue;
            const std::string& cc   = c[0];
            const std::string& city = c[1];
            const std::string& adm1 = c[3];
            std::string city_key = cc + ":" + city;

            // Prefer direct (Strategy A/B/C) over admin1 bucket
            auto dit = direct_city_map.find(city_key);
            if (dit != direct_city_map.end() && !dit->second.empty()) {
                std::string val;
                for (const auto& pc : dit->second) {
                    if (!val.empty()) val += ','; val += pc;
                }
                db.put(city_key, val);
                continue;
            }

            // Fall back to admin1 bucket
            auto ait = admin_map.find(cc + ":" + adm1);
            if (ait == admin_map.end() || ait->second.empty()) continue;
            std::string val;
            for (const auto& pc : ait->second) {
                if (!val.empty()) val += ','; val += pc;
            }
            db.put(city_key, val);
        }
    }

    // Write remaining direct entries not in cities CSV
    for (const auto& [key, pcs] : direct_city_map) {
        if (pcs.empty()) continue;
        std::string val;
        for (const auto& pc : pcs) { if (!val.empty()) val += ','; val += pc; }
        db.put(key, val);
    }

    db.commit();
}

// =============================================================================
//  build_cities
// =============================================================================
static void build_cities(const std::string& csv, const std::string& lmdb_dir) {
    std::cout << "[build_lmdb] Building cities.lmdb (3M+ entries)...\n";
    LmdbWriter db(lmdb_dir + "/cities.lmdb", 512);
    std::ifstream f(csv);
    if (!f.is_open()) throw std::runtime_error("Cannot open: " + csv);
    std::string line; std::getline(f, line);
    while (std::getline(f, line)) {
        auto c = parseCsvLine(line);
        if (c.size() < 3 || c[0].empty() || c[1].empty() || c[2].empty()) continue;
        db.put(c[0] + ":" + c[1], c[2]);
    }
    db.commit();
}

// =============================================================================
//  build_countries
// =============================================================================
static void build_countries(const std::string& csv, const std::string& lmdb_dir) {
    std::cout << "[build_lmdb] Building countries.lmdb...\n";
    LmdbWriter db(lmdb_dir + "/countries.lmdb", 16);
    std::ifstream f(csv);
    if (!f.is_open()) throw std::runtime_error("Cannot open: " + csv);
    std::string line; std::getline(f, line);
    while (std::getline(f, line)) {
        auto c = parseCsvLine(line);
        if (c.size() < 4 || c[0].empty() || c[3].empty()) continue;
        std::string val = c[3] + "|" + c[0] + "|" + c[1] + "|" + c[2] + "|" + c[4] + "|" + c[5];
        db.put(c[0], val);
        if (!c[1].empty()) db.put(c[1], val);
        if (!c[2].empty()) db.put(c[2], val);
        db.put(c[3], val);
    }
    db.commit();
}

// =============================================================================
//  build_aliases
// =============================================================================
static void build_aliases(const std::string& csv, const std::string& lmdb_dir) {
    std::cout << "[build_lmdb] Building aliases.lmdb...\n";
    LmdbWriter db(lmdb_dir + "/aliases.lmdb", 16);
    std::ifstream f(csv);
    if (!f.is_open()) throw std::runtime_error("Cannot open: " + csv);
    std::string line; std::getline(f, line);
    while (std::getline(f, line)) {
        auto c = parseCsvLine(line);
        if (c.size() < 3 || c[0].empty() || c[1].empty() || c[2].empty()) continue;
        db.put(c[2] + ":" + c[0], c[1]);
    }
    db.commit();
}

// =============================================================================
//  Main
// =============================================================================
int main(int argc, char* argv[]) {
    std::string csv_dir, lmdb_dir;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--csv-dir"  && i+1 < argc) csv_dir  = argv[++i];
        if (arg == "--lmdb-dir" && i+1 < argc) lmdb_dir = argv[++i];
    }
    if (csv_dir.empty() || lmdb_dir.empty()) {
        std::cerr << "Usage: build_lmdb --csv-dir <path> --lmdb-dir <path>\n";
        return 1;
    }
    auto expandHome = [](std::string p) {
        if (!p.empty() && p[0] == '~') {
            const char* h = std::getenv("HOME");
            if (h) p = std::string(h) + p.substr(1);
        }
        return p;
    };
    csv_dir  = expandHome(csv_dir);
    lmdb_dir = expandHome(lmdb_dir);
    fs::create_directories(lmdb_dir);

    std::cout << "[build_lmdb] CSV dir  : " << csv_dir  << "\n";
    std::cout << "[build_lmdb] LMDB dir : " << lmdb_dir << "\n\n";

    auto t0 = std::chrono::steady_clock::now();
    try {
        build_admin1        (csv_dir + "/admin1_filtered.csv",       lmdb_dir);
        build_admin2        (csv_dir + "/admin2_filtered.csv",       lmdb_dir);
        build_postal        (csv_dir + "/postal_filtered.csv",       lmdb_dir);
        build_postal_reverse(csv_dir + "/postal_filtered.csv",
                             csv_dir + "/cities_filtered.csv",       lmdb_dir);
        build_cities        (csv_dir + "/cities_filtered.csv",       lmdb_dir);
        build_countries     (csv_dir + "/countries_filtered.csv",    lmdb_dir);
        build_aliases       (csv_dir + "/city_aliases_filtered.csv", lmdb_dir);
    } catch (const std::exception& e) {
        std::cerr << "[build_lmdb] ERROR: " << e.what() << "\n";
        return 1;
    }

    double s = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    std::cout << "\n[build_lmdb] Done in " << s << "s\n";
    std::cout << "[build_lmdb] LMDB files: " << lmdb_dir << "\n";
    return 0;
}