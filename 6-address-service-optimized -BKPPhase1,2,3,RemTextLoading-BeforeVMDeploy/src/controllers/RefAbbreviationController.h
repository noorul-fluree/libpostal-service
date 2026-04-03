#pragma once

#include <drogon/HttpController.h>
#include "controllers/AuthFilter.h"

namespace addr {

// =============================================================================
//  RefAbbreviationController
//
//  POST /ref/v1/abbreviation/expand
//
//  Request:
//  {
//    "text": "123 MG RD, BLDG 4, Bengaluru"
//  }
//
//  Response:
//  {
//    "input":    "123 MG RD, BLDG 4, Bengaluru",
//    "expanded": "123 mahatma gandhi road building 4 bengaluru",
//    "changed":  true
//  }
//
//  Runs the full PreProcessor pipeline (trim + lowercase + abbreviation
//  expansion) — identical to what the address pipeline applies internally.
// =============================================================================
class RefAbbreviationController
    : public drogon::HttpController<RefAbbreviationController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(RefAbbreviationController::expand,
                  "/ref/v1/abbreviation/expand",
                  drogon::Post, "addr::AuthFilter");
    METHOD_LIST_END

    void expand(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb);
};

} // namespace addr