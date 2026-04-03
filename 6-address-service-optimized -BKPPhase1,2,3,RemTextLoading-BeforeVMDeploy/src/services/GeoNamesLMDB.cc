#include "services/GeoNamesLMDB.h"
#include <iostream>
#include <algorithm>
#include <cctype>
#include <sstream>

namespace addr {

std::string GeoNamesLMDB::normalize(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return r;
    size_t b = s.find_last_not_of(" \t\r\n");
    for (size_t i = a; i <= b; ++i)
        r += static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    return r;
}

bool GeoNamesLMDB::openHandle(const std::string& path, LmdbHandle& h) {
    int rc = mdb_env_create(&h.env);
    if (rc != MDB_SUCCESS) {
        std::cerr << "[GeoNamesLMDB] mdb_env_create: " << mdb_strerror(rc) << "\n";
        return false;
    }
    rc = mdb_env_open(h.env, path.c_str(), MDB_RDONLY, 0664);
    if (rc != MDB_SUCCESS) {
        std::cerr << "[GeoNamesLMDB] open(" << path << "): " << mdb_strerror(rc) << "\n";
        mdb_env_close(h.env); h.env = nullptr;
        return false;
    }
    MDB_txn* txn = nullptr;
    rc = mdb_txn_begin(h.env, nullptr, MDB_RDONLY, &txn);
    if (rc != MDB_SUCCESS) {
        mdb_env_close(h.env); h.env = nullptr; return false;
    }
    rc = mdb_dbi_open(txn, nullptr, 0, &h.dbi);
    mdb_txn_abort(txn);
    if (rc != MDB_SUCCESS) {
        mdb_env_close(h.env); h.env = nullptr; return false;
    }
    return true;
}

void GeoNamesLMDB::closeHandle(LmdbHandle& h) {
    if (h.env) { mdb_env_close(h.env); h.env = nullptr; }
}

bool GeoNamesLMDB::initialize(const std::string& lmdb_dir) {
    std::string dir = lmdb_dir;
    if (!dir.empty() && dir.back() != '/') dir += '/';
    lmdb_dir_ = lmdb_dir;

    std::cout << "[GeoNamesLMDB] Opening 7 LMDB databases from " << dir << "\n";

    if (!openHandle(dir + "admin1.lmdb",         h_admin1_))        return false;
    if (!openHandle(dir + "admin2.lmdb",         h_admin2_))        return false;
    if (!openHandle(dir + "postal.lmdb",         h_postal_))        return false;
    if (!openHandle(dir + "postal_reverse.lmdb", h_postal_reverse_))return false;
    if (!openHandle(dir + "cities.lmdb",         h_cities_))        return false;
    if (!openHandle(dir + "countries.lmdb",      h_countries_))     return false;
    if (!openHandle(dir + "aliases.lmdb",        h_aliases_))       return false;

    ready_.store(true);
    std::cout << "[GeoNamesLMDB] Ready — "
              << "admin1 + admin2 + postal + postal_reverse "
              << "+ cities + countries + aliases\n";
    return true;
}

void GeoNamesLMDB::shutdown() {
    ready_.store(false);
    closeHandle(h_admin1_);
    closeHandle(h_admin2_);
    closeHandle(h_postal_);
    closeHandle(h_postal_reverse_);
    closeHandle(h_cities_);
    closeHandle(h_countries_);
    closeHandle(h_aliases_);
}

std::string GeoNamesLMDB::lookup(const LmdbHandle& h,
                                   const std::string& key) const {
    if (!h.env) return {};
    MDB_txn* txn = nullptr;
    if (mdb_txn_begin(h.env, nullptr, MDB_RDONLY, &txn) != MDB_SUCCESS) return {};
    MDB_val k, v;
    k.mv_data = const_cast<char*>(key.data()); k.mv_size = key.size();
    std::string result;
    if (mdb_get(txn, h.dbi, &k, &v) == MDB_SUCCESS)
        result.assign(static_cast<char*>(v.mv_data), v.mv_size);
    mdb_txn_abort(txn);
    return result;
}

// =============================================================================
//  lookupPostal — Phase 2A
// =============================================================================
std::optional<GeoEntry>
GeoNamesLMDB::lookupPostal(const std::string& postal_code,
                             const std::string& country_code) const {
    if (!ready_.load(std::memory_order_relaxed)) return std::nullopt;
    if (postal_code.empty() || country_code.empty()) return std::nullopt;

    std::string pc;
    if (country_code == "GB") {
        pc = postal_code;
        std::transform(pc.begin(), pc.end(), pc.begin(), ::toupper);
        auto sp = pc.find(' ');
        if (sp != std::string::npos) pc = pc.substr(0, sp);
    } else {
        pc = normalize(postal_code);
    }

    std::string val = lookup(h_postal_, country_code + ":" + pc);
    if (val.empty()) return std::nullopt;

    auto pipe = val.find('|');
    GeoEntry e;
    e.country_code = country_code;
    if (pipe != std::string::npos) {
        e.city  = val.substr(0, pipe);
        e.state = val.substr(pipe + 1);
    } else { e.city = val; }
    return e;
}

// =============================================================================
//  lookupPostalFull
// =============================================================================
std::optional<PostalRecord>
GeoNamesLMDB::lookupPostalFull(const std::string& postal_code,
                                 const std::string& country_code) const {
    auto entry = lookupPostal(postal_code, country_code);
    if (!entry) return std::nullopt;
    PostalRecord r;
    r.country_code = country_code;
    r.postal_code  = postal_code;
    r.city         = entry->city;
    r.state        = entry->state;
    return r;
}

// =============================================================================
//  lookupPostalReverse — city → list of postal codes
//  Returns up to REVERSE_MAX_RESULTS codes, sorted.
//  total_stored reflects how many are actually in the database (capped at 50
//  by the builder — if 50 are stored, true count may be higher).
// =============================================================================
GeoNamesLMDB::ReverseResult
GeoNamesLMDB::lookupPostalReverse(const std::string& city,
                                    const std::string& country_code) const {
    ReverseResult result;
    if (!ready_.load(std::memory_order_relaxed)) return result;
    if (city.empty() || country_code.empty()) return result;

    // Try canonical city name first (resolve alias)
    std::string norm = normalize(city);
    std::string canonical = lookup(h_aliases_, country_code + ":" + norm);
    if (!canonical.empty()) norm = canonical;

    std::string val = lookup(h_postal_reverse_, country_code + ":" + norm);
    if (val.empty()) return result;

    // Parse comma-separated postal codes
    std::istringstream ss(val);
    std::string pc;
    while (std::getline(ss, pc, ',')) {
        if (!pc.empty()) result.postcodes.push_back(pc);
    }

    result.total_stored = static_cast<int>(result.postcodes.size());

    // Return max REVERSE_MAX_RESULTS
    if (static_cast<int>(result.postcodes.size()) > REVERSE_MAX_RESULTS)
        result.postcodes.resize(REVERSE_MAX_RESULTS);

    return result;
}

// =============================================================================
//  lookupCityState — Phase 2B
// =============================================================================
std::string GeoNamesLMDB::lookupCityState(const std::string& city,
                                            const std::string& country_code) const {
    if (!ready_.load(std::memory_order_relaxed)) return {};
    if (city.empty() || country_code.empty()) return {};
    return lookup(h_cities_, country_code + ":" + normalize(city));
}

// =============================================================================
//  lookupCityStateFull — alias first, then direct
// =============================================================================
std::string GeoNamesLMDB::lookupCityStateFull(const std::string& city,
                                                const std::string& country_code) const {
    if (!ready_.load(std::memory_order_relaxed)) return {};
    std::string norm = normalize(city);
    std::string canonical = lookup(h_aliases_, country_code + ":" + norm);
    if (!canonical.empty()) norm = canonical;
    return lookup(h_cities_, country_code + ":" + norm);
}

// =============================================================================
//  lookupCountry
// =============================================================================
std::optional<CountryRecord>
GeoNamesLMDB::lookupCountry(const std::string& code_or_name) const {
    if (!ready_.load(std::memory_order_relaxed)) return std::nullopt;
    if (code_or_name.empty()) return std::nullopt;

    std::string upper = code_or_name;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

    std::string val = lookup(h_countries_, upper);
    if (val.empty())
        val = lookup(h_countries_, normalize(code_or_name));
    if (val.empty()) return std::nullopt;

    std::vector<std::string> parts;
    std::istringstream ss(val);
    std::string p;
    while (std::getline(ss, p, '|')) parts.push_back(p);

    CountryRecord r;
    r.name         = parts.size() > 0 ? parts[0] : "";
    r.alpha2       = parts.size() > 1 ? parts[1] : "";
    r.alpha3       = parts.size() > 2 ? parts[2] : "";
    r.numeric_code = parts.size() > 3 ? parts[3] : "";
    r.region       = parts.size() > 4 ? parts[4] : "";
    r.sub_region   = parts.size() > 5 ? parts[5] : "";
    return r;
}

// =============================================================================
//  lookupState
// =============================================================================
std::string GeoNamesLMDB::lookupState(const std::string& cc,
                                        const std::string& adm1) const {
    if (!ready_.load(std::memory_order_relaxed)) return {};
    return lookup(h_admin1_, cc + ":" + adm1);
}

// =============================================================================
//  lookupDistrict
// =============================================================================
std::string GeoNamesLMDB::lookupDistrict(const std::string& cc,
                                           const std::string& adm1,
                                           const std::string& adm2) const {
    if (!ready_.load(std::memory_order_relaxed)) return {};
    return lookup(h_admin2_, cc + ":" + adm1 + ":" + adm2);
}

// =============================================================================
//  lookupAlias
// =============================================================================
std::string GeoNamesLMDB::lookupAlias(const std::string& city,
                                        const std::string& cc) const {
    if (!ready_.load(std::memory_order_relaxed)) return {};
    return lookup(h_aliases_, cc + ":" + normalize(city));
}

} // namespace addr