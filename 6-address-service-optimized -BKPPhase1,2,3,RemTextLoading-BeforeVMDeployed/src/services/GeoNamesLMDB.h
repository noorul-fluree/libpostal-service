#pragma once

#include <string>
#include <optional>
#include <atomic>
#include <vector>
#include <lmdb.h>

namespace addr {

// =============================================================================
//  GeoEntry — result of a geo lookup
//  Previously in GeoNamesDB.h — moved here since GeoNamesDB is removed
// =============================================================================
struct GeoEntry {
    std::string city;
    std::string state;
    std::string country_code;
};

// =============================================================================
//  CountryRecord — result of a country lookup
// =============================================================================
struct CountryRecord {
    std::string alpha2;
    std::string alpha3;
    std::string numeric_code;
    std::string name;
    std::string region;
    std::string sub_region;
};

// =============================================================================
//  PostalRecord — full result of a postal lookup
// =============================================================================
struct PostalRecord {
    std::string country_code;
    std::string postal_code;
    std::string city;
    std::string state;
};

// =============================================================================
//  GeoNamesLMDB — LMDB-backed geo lookup (all countries)
//
//  7 databases (all read-only at runtime):
//    admin1.lmdb         "IN:19"        → "karnataka"
//    admin2.lmdb         "IN:19:1234"   → "bengaluru urban"
//    postal.lmdb         "IN:560001"    → "bengaluru|karnataka"
//    postal_reverse.lmdb "IN:bengaluru" → "560001,560002,560003,..."
//    cities.lmdb         "IN:bengaluru" → "karnataka"
//    countries.lmdb      "IN"           → "india|IN|IND|356|asia|southern asia"
//    aliases.lmdb        "IN:bombay"    → "mumbai"
// =============================================================================
class GeoNamesLMDB {
public:
    static constexpr double GEO_OVERRIDE_THRESHOLD = 0.7;
    static constexpr int    REVERSE_MAX_RESULTS    = 20;

    static GeoNamesLMDB& instance() {
        static GeoNamesLMDB inst;
        return inst;
    }

    bool initialize(const std::string& lmdb_dir);
    void shutdown();
    bool isReady() const { return ready_.load(std::memory_order_relaxed); }
    const std::string& lmdbDir() const { return lmdb_dir_; }

    // Phase 2A: postal → city + state
    std::optional<GeoEntry> lookupPostal(const std::string& postal_code,
                                          const std::string& country_code) const;

    // Phase 2B: city → state
    std::string lookupCityState(const std::string& city,
                                 const std::string& country_code) const;

    // Full postal record
    std::optional<PostalRecord> lookupPostalFull(const std::string& postal_code,
                                                  const std::string& country_code) const;

    // Reverse postal lookup: city → list of postal codes (max REVERSE_MAX_RESULTS)
    // Returns {postcodes, total_stored} — total_stored may be > postcodes.size()
    // if the city has more than 50 codes in the database
    struct ReverseResult {
        std::vector<std::string> postcodes;
        int total_stored = 0;
    };
    ReverseResult lookupPostalReverse(const std::string& city,
                                       const std::string& country_code) const;

    // Country lookup (alpha2, alpha3, numeric, or name)
    std::optional<CountryRecord> lookupCountry(const std::string& code_or_name) const;

    // State lookup: country + admin1_code → state name
    std::string lookupState(const std::string& country_code,
                             const std::string& admin1_code) const;

    // District lookup: country + admin1 + admin2 → district name
    std::string lookupDistrict(const std::string& country_code,
                                const std::string& admin1_code,
                                const std::string& admin2_code) const;

    // City alias: "bombay" + "IN" → "mumbai"
    std::string lookupAlias(const std::string& city,
                             const std::string& country_code) const;

    // City search — find state for a city (tries alias first, then direct)
    std::string lookupCityStateFull(const std::string& city,
                                     const std::string& country_code) const;

private:
    GeoNamesLMDB() = default;
    ~GeoNamesLMDB() { shutdown(); }

    struct LmdbHandle {
        MDB_env* env = nullptr;
        MDB_dbi  dbi = 0;
    };

    LmdbHandle h_admin1_;
    LmdbHandle h_admin2_;
    LmdbHandle h_postal_;
    LmdbHandle h_postal_reverse_;
    LmdbHandle h_cities_;
    LmdbHandle h_countries_;
    LmdbHandle h_aliases_;

    std::string lmdb_dir_;
    std::atomic<bool> ready_{false};

    bool openHandle(const std::string& path, LmdbHandle& h);
    void closeHandle(LmdbHandle& h);
    std::string lookup(const LmdbHandle& h, const std::string& key) const;
    static std::string normalize(const std::string& s);
};

} // namespace addr