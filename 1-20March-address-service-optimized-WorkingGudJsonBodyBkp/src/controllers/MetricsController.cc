#include "controllers/MetricsController.h"
#include "services/MetricsCollector.h"

namespace addr {

void MetricsController::metrics(const drogon::HttpRequestPtr& req,
                                 std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    auto& collector = MetricsCollector::instance();
    std::string body = collector.serialize();

    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
    resp->setBody(body);
    callback(resp);
}

} // namespace addr
