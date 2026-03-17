#pragma once

#include <drogon/HttpController.h>

namespace addr {

class ParseController : public drogon::HttpController<ParseController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(ParseController::parse, "/api/v1/parse", drogon::Post);
    ADD_METHOD_TO(ParseController::normalize, "/api/v1/normalize", drogon::Post);
    METHOD_LIST_END

    void parse(const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void normalize(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& callback);
};

} // namespace addr
