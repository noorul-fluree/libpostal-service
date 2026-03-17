#pragma once

#include <drogon/HttpController.h>

namespace addr {

class MetricsController : public drogon::HttpController<MetricsController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(MetricsController::metrics, "/metrics", drogon::Get);
    METHOD_LIST_END

    void metrics(const drogon::HttpRequestPtr& req,
                 std::function<void(const drogon::HttpResponsePtr&)>&& callback);
};

} // namespace addr
