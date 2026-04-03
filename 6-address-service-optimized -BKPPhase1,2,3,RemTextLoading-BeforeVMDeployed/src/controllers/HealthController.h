#pragma once

#include <drogon/HttpController.h>

namespace addr {

class HealthController : public drogon::HttpController<HealthController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(HealthController::live,    "/health/live",    drogon::Get);
    ADD_METHOD_TO(HealthController::ready,   "/health/ready",   drogon::Get);
    ADD_METHOD_TO(HealthController::startup, "/health/startup", drogon::Get);
    ADD_METHOD_TO(HealthController::info,    "/health/info",    drogon::Get);
    METHOD_LIST_END

    void live(const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void ready(const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void startup(const drogon::HttpRequestPtr& req,
                 std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void info(const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& callback);
};

} // namespace addr
