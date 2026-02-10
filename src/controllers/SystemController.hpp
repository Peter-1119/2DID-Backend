#pragma once
#include "crow_all.h"
#include "../services/MesService.hpp"
#include "../core/AppState.hpp"

class SystemController {
public:
    static void init(crow::App<crow::CORSHandler>& app, std::shared_ptr<MesService> mesService) {
        
        // API 1: Heartbeat
        CROW_ROUTE(app, "/heartbeat").methods(crow::HTTPMethod::Get)
        ([]() {
            return crow::response(json{{"MES_alive", AppState::isMesOnline.load()}}.dump());
        });

        // API 2: Validate Employee
        CROW_ROUTE(app, "/api/validate-emp").methods(crow::HTTPMethod::Post)
        ([mesService](const crow::request& req){
            try {
                auto x = json::parse(req.body);
                std::string empId = x.value("empId", "");

                // ✅ 改為同步呼叫
                std::string result = mesService->validate_employee_iis(empId);
                
                crow::response res;
                if (!result.empty()) {
                    res.code = 200;
                    res.add_header("Content-Type", "application/json");
                    res.write(result);
                } else {
                    res.code = 502;
                    res.write(json{{"success", false}, {"message", "IIS Error"}}.dump());
                }
                return res;
            } catch (...) {
                return crow::response(400, "Invalid JSON");
            }
        });

        // API 10: Get Machine Info
        CROW_ROUTE(app, "/api/get-machine-info").methods(crow::HTTPMethod::Post)
        ([mesService](const crow::request& req){
            try {
                auto x = json::parse(req.body);
                std::string machine_id = x.value("machine_id", "");
                std::string emp_no = x.value("emp_no", "");

                // ✅ 改為同步呼叫
                auto info = mesService->get_machine_cameras(emp_no, machine_id);
                
                crow::response res;
                if (!info.empty()) {
                    res.code = 200;
                    res.write(json{{"success", true}, {"data", info}}.dump());
                } else {
                    res.code = 404;
                    res.write(json{{"success", false}, {"message", "CMD 254 Failed or Empty"}}.dump());
                }
                return res;
            } catch (...) {
                return crow::response(400, "Invalid JSON");
            }
        });
    }
};