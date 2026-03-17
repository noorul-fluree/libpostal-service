#pragma once

#include <drogon/HttpController.h>

namespace addr {

class BatchController : public drogon::HttpController<BatchController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(BatchController::batchParse, "/api/v1/batch", drogon::Post);
    METHOD_LIST_END

    void batchParse(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& callback);
};

} // namespace addr
