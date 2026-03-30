#include "services/GeoNamesDB.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cctype>

namespace addr {

// =============================================================================
//  normalize — lowercase + trim whitespace
// =============================================================================
std::string GeoNamesDB::normalize(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    // trim leading
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return r;
    size_t end = s.find_last_not_of(" \t\r\n");
    for (size_t i = start; i <= end; ++i)
        r += static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    return r;
}

// =============================================================================
//  initialize
// =============================================================================
bool GeoNamesDB::initialize(const std::string& data_dir) {
    std::string dir = data_dir;
    if (!dir.empty() && dir.back() != '/') dir += '/';

    std::cout << "[GeoNamesDB] Loading data from " << dir << "\n";

    if (!loadAdmin1Codes(dir + "admin1CodesASCII.txt")) return false;
    if (!loadPostalCodes(dir + "postal_codes.txt"))     return false;
    if (!loadCities(dir      + "cities15000.txt"))      return false;

    ready_.store(true);
    std::cout << "[GeoNamesDB] Ready"
              << " | postal_entries=" << postal_map_.size()
              << " | city_entries="   << city_state_map_.size()
              << "\n";
    return true;
}

// =============================================================================
//  loadAdmin1Codes
//  Format: CC.admin1code\tname\tascii_name\tgeonameid
//  Example: IN.07\tGujarat\tGujarat\t1270835
// =============================================================================
bool GeoNamesDB::loadAdmin1Codes(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "[GeoNamesDB] ERROR: cannot open " << path << "\n";
        return false;
    }

    std::string line;
    int count = 0;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        // split on tab
        std::vector<std::string> cols;
        std::istringstream ss(line);
        std::string col;
        while (std::getline(ss, col, '\t')) cols.push_back(col);
        if (cols.size() < 2) continue;

        // cols[0] = "IN.07", cols[1] = "Gujarat"
        const std::string& code_str = cols[0]; // e.g. "IN.07"
        auto dot = code_str.find('.');
        if (dot == std::string::npos) continue;

        std::string cc      = code_str.substr(0, dot);
        std::string adm1    = code_str.substr(dot + 1);
        std::string state   = normalize(cols[1]);

        if (!isTargetCountry(cc)) continue;

        // key = "IN:07"
        admin1_map_[cc + ":" + adm1] = state;
        ++count;
    }

    std::cout << "[GeoNamesDB] admin1 codes loaded: " << count << "\n";
    return true;
}

// =============================================================================
//  loadPostalCodes
//  Format (tab-separated):
//    col0: country_code
//    col1: postal_code
//    col2: place_name
//    col3: admin1_name
//    col4: admin1_code
//  Example: IN\t560001\tBengaluru\tKarnataka\t19
// =============================================================================
bool GeoNamesDB::loadPostalCodes(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "[GeoNamesDB] ERROR: cannot open " << path << "\n";
        return false;
    }

    std::string line;
    int count = 0;
    while (std::getline(f, line)) {
        if (line.empty()) continue;

        // Manual tab split — faster than istringstream for large files
        std::vector<std::string_view> cols;
        cols.reserve(12);
        size_t start = 0;
        for (size_t i = 0; i <= line.size(); ++i) {
            if (i == line.size() || line[i] == '\t') {
                cols.push_back(std::string_view(line.data() + start, i - start));
                start = i + 1;
            }
        }
        if (cols.size() < 5) continue;

        std::string cc     = std::string(cols[0]);
        if (!isTargetCountry(cc)) continue;

        // For GB: keep postal code uppercase (outward code format)
        // For others: normalize to lowercase
        std::string postal;
        if (cc == "GB") {
            postal = std::string(cols[1]);
            std::transform(postal.begin(), postal.end(), postal.begin(), ::toupper);
        } else {
            postal = normalize(std::string(cols[1]));
        }
        std::string place_name = normalize(std::string(cols[2]));
        std::string admin1_name= normalize(std::string(cols[3]));
        std::string admin1_code= std::string(cols[4]);

        if (postal.empty()) continue;

        // Resolve state: prefer admin1 name from this file directly
        std::string state = admin1_name;
        // Also try admin1 map as fallback
        if (state.empty()) {
            auto it = admin1_map_.find(cc + ":" + admin1_code);
            if (it != admin1_map_.end()) state = it->second;
        }

        std::string key = cc + ":" + postal;
        // Only insert first occurrence (highest accuracy entry)
        if (postal_map_.find(key) == postal_map_.end()) {
            postal_map_[key] = {place_name, state, cc};
            ++count;
        }
    }

    std::cout << "[GeoNamesDB] postal codes loaded: " << count << "\n";
    return true;
}

// =============================================================================
//  loadCities
//  Format (tab-separated, 19 cols):
//    col1:  name (city)
//    col7:  feature_class (P = populated place)
//    col8:  country_code
//    col10: admin1_code
//  Only load feature_class = P (populated places)
// =============================================================================
bool GeoNamesDB::loadCities(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "[GeoNamesDB] ERROR: cannot open " << path << "\n";
        return false;
    }

    std::string line;
    int count = 0;
    while (std::getline(f, line)) {
        if (line.empty()) continue;

        std::vector<std::string_view> cols;
        cols.reserve(19);
        size_t start = 0;
        for (size_t i = 0; i <= line.size(); ++i) {
            if (i == line.size() || line[i] == '\t') {
                cols.push_back(std::string_view(line.data() + start, i - start));
                start = i + 1;
            }
        }
        if (cols.size() < 11) continue;

        std::string cc = std::string(cols[8]);
        if (!isTargetCountry(cc)) continue;

        // Only populated places
        std::string_view feat = cols[7];
        if (feat != "P" && feat != "PPL" && feat != "PPLA" &&
            feat != "PPLA2" && feat != "PPLC" && feat != "PPLX") {
            // feature_class is col7, feature_code is also col7 in cities15000
            // cities15000 uses feature_code directly — accept all P* codes
            if (feat.empty() || feat[0] != 'P') continue;
        }

        std::string city       = normalize(std::string(cols[1]));
        std::string admin1_code= std::string(cols[10]);

        if (city.empty()) continue;

        // Resolve state name from admin1 map
        std::string state;
        auto it = admin1_map_.find(cc + ":" + admin1_code);
        if (it != admin1_map_.end()) state = it->second;
        if (state.empty()) continue; // skip if no state mapping

        std::string key = cc + ":" + city;
        // Only insert first (most populated — cities15000 is sorted by pop desc)
        if (city_state_map_.find(key) == city_state_map_.end()) {
            city_state_map_[key] = state;
            ++count;
        }
    }

    std::cout << "[GeoNamesDB] cities loaded: " << count << "\n";
    return true;
}

// =============================================================================
//  lookupPostal — Phase 2A
// =============================================================================
std::optional<GeoEntry>
GeoNamesDB::lookupPostal(const std::string& postal_code,
                          const std::string& country_code) const {
    if (postal_code.empty() || country_code.empty()) return std::nullopt;

    // GB postcodes stored uppercase in GeoNames — normalize accordingly
    std::string pc;
    if (country_code == "GB") {
        pc = postal_code;
        std::transform(pc.begin(), pc.end(), pc.begin(), ::toupper);
        pc.erase(std::remove(pc.begin(), pc.end(), ' '), pc.end());
    } else {
        pc = normalize(postal_code);
    }

    std::string key = country_code + ":" + pc;
    auto it = postal_map_.find(key);
    if (it == postal_map_.end()) return std::nullopt;
    return it->second;
}

// =============================================================================
//  lookupCityState — Phase 2B
// =============================================================================
std::string GeoNamesDB::lookupCityState(const std::string& city,
                                         const std::string& country_code) const {
    if (city.empty() || country_code.empty()) return {};

    std::string key = country_code + ":" + normalize(city);
    auto it = city_state_map_.find(key);
    if (it == city_state_map_.end()) return {};
    return it->second;
}

} // namespace addr