#pragma once

#include "models/AddressModels.h"

namespace addr {

class ConfidenceScorer {
public:
    // Compute confidence score (0.0 - 1.0) for a parsed address
    double score(const ParsedAddress& parsed) const;

    // Get a breakdown of individual scores for debugging
    struct ScoreBreakdown {
        double completeness = 0.0;      // How many fields are populated
        double postcode_validity = 0.0;  // Is the postcode well-formed
        double cross_field = 0.0;        // Do fields agree with each other
        double token_coverage = 0.0;     // How many input tokens were assigned
        double total = 0.0;
    };

    ScoreBreakdown scoreDetailed(const ParsedAddress& parsed) const;

private:
    double scoreCompleteness(const ParsedAddress& p) const;
    double scorePostcodeValidity(const ParsedAddress& p) const;
    double scoreCrossField(const ParsedAddress& p) const;
    double scoreTokenCoverage(const ParsedAddress& p) const;
};

} // namespace addr
