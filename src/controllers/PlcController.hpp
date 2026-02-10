#pragma once
#include "crow_all.h"
#include "../services/PlcService.hpp"

class PlcController {
public:
    static void init(crow::App<crow::CORSHandler>& app, std::shared_ptr<PlcService> service) {
        
        // ✅ 使用 req, res 的 Callback 模式，這是完全 Non-blocking 的
        CROW_ROUTE(app, "/api/write-PLC-status").methods(crow::HTTPMethod::Post)
        ([service](const crow::request& req, crow::response& res) {
            auto x = json::parse(req.body);
            std::string machine_id = x.value("machine_id", "");
            std::string status = x.value("status", "");

            if (machine_id.empty()) {
                res.code = 400;
                res.end();
                return;
            }

            // 直接傳遞 res 給 service，不再等待
            // 注意：Crow 的非同步 res 需要手動呼叫 res.end()
            service->execute_write_status(machine_id, status, [&res](std::string result) {
                res.code = 200;
                res.write(json{{"result", result}}.dump());
                res.end(); // 真正完成請求 
            });
        });
    }
};