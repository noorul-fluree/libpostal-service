#pragma once

#include <string>
#include <unordered_map>
#include <optional>
#include <atomic>

namespace addr {

// =============================================================================
//  GeoEntry — result of a geo lookup
// =============================================================================
struct GeoEntry {
    std::string city;
    std::string state;
    std::string country_code;
};

// =============================================================================
//  GeoNamesDB — in-memory lookup tables built from GeoNames data files
//
//  Phase 2A: postal_code + country_code → GeoEntry
//    Source: postal_codes.txt (from GeoNames /export/zip/allCountries.zip)
//    Key:    "IN:560001"
//
//  Phase 2B: city_lower + country_code → state name
//    Source: cities15000.txt (GeoNames cities with population > 15000)
//    Key:    "IN:bengaluru"
//
//  Countries loaded: IN (India), US (United States), GB (United Kingdom)
//  All keys are lowercase for case-insensitive lookup.
//
//  Fill strategy:
//    - Fill blank fields unconditionally
//    - Override existing field if confidence < GEO_OVERRIDE_THRESHOLD (0.7)
// =============================================================================
class GeoNamesDB {
public:
    static constexpr double GEO_OVERRIDE_THRESHOLD = 0.7;

    // Singleton
    static GeoNamesDB& instance() {
        static GeoNamesDB inst;
        return inst;
    }

    // Load all data files. Call once at startup.
    // data_dir = path to directory containing postal_codes.txt,
    //            cities15000.txt, admin1CodesASCII.txt
    bool initialize(const std::string& data_dir);

    bool isReady() const { return ready_.load(std::memory_order_relaxed); }

    // Phase 2A: postal code + country → city + state
    // Returns nullopt if not found
    std::optional<GeoEntry> lookupPostal(const std::string& postal_code,
                                          const std::string& country_code) const;

    // Phase 2B: city name + country → state
    // Returns empty string if not found
    std::string lookupCityState(const std::string& city,
                                 const std::string& country_code) const;

    // Stats
    size_t postalEntries() const { return postal_map_.size(); }
    size_t cityEntries()   const { return city_state_map_.size(); }

private:
    GeoNamesDB() = default;

    // Phase 2A map: "IN:560001" → GeoEntry
    std::unordered_map<std::string, GeoEntry> postal_map_;

    // Phase 2B map: "IN:bengaluru" → "karnataka"
    std::unordered_map<std::string, std::string> city_state_map_;

    // admin1 code → state name: "IN:07" → "gujarat"
    std::unordered_map<std::string, std::string> admin1_map_;

    std::atomic<bool> ready_{false};

    // Loaders
    bool loadAdmin1Codes(const std::string& path);
    bool loadPostalCodes(const std::string& path);
    bool loadCities(const std::string& path);

    // Target countries only
    static bool isTargetCountry(const std::string& cc) {
        return cc == "IN" || cc == "US" || cc == "GB";
    }

    // Normalize: lowercase, trim
    static std::string normalize(const std::string& s);
};

} // namespace addr