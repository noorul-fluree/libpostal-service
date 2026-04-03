#include "controllers/RefAbbreviationController.h"
#include "services/PreProcessor.h"
#include "utils/Logger.h"

extern addr::PreProcessor g_preprocessor;

namespace addr {

void RefAbbreviationController::expand(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& cb)
{
    auto json = req->getJsonObject();
    if (!json || !json->isMember("text") || !(*json)["text"].isString()) {
        Json::Value err;
        err["error"] = "Missing or invalid 'text' field";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k400BadRequest);
        cb(r); return;
    }

    std::string input = (*json)["text"].asString();

    if (input.empty()) {
        Json::Value err;
        err["error"] = "text field is empty";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k400BadRequest);
        cb(r); return;
    }

    // Run full pipeline: trim + lowercase + abbreviation expand
    std::string expanded = g_preprocessor.process(input);
    bool changed = (expanded != input);

    LOG_F(INFO, "[ref/abbreviation/expand] input_len=%d changed=%s",
          (int)input.size(), changed ? "true" : "false");

    Json::Value out;
    out["input"]    = input;
    out["expanded"] = expanded;
    out["changed"]  = changed;

    cb(drogon::HttpResponse::newHttpJsonResponse(out));
}

} // namespace addr