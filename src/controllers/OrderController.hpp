#pragma once
#include "crow_all.h"
#include "../services/OrderService.hpp"

class OrderController {
public:
    static void init(crow::App<crow::CORSHandler>& app, std::shared_ptr<OrderService> service) {
        
        // ==========================================
        // API: Get WorkOrder (查詢工單)
        // ==========================================
        CROW_ROUTE(app, "/api/get-workorder").methods(crow::HTTPMethod::Post)
        ([service](const crow::request& req, crow::response& res) {
            auto x = json::parse(req.body);
            std::string wo = x.value("workorder", "");
            std::string emp = x.value("emp_no", "");
            bool insertDB = x.value("insert_to_database", false);
            // ✅ [改為同步] 直接回傳 response 物件，不再傳入 &res
            try {
                spdlog::info("[API] Processing WO: {}", wo);
                
                // 直接呼叫業務邏輯 (Running in Crow's Worker Thread)
                json result = service->get_work_order(wo, emp, insertDB);
                
                crow::response res;
                res.code = 200;
                res.add_header("Content-Type", "application/json");
                res.write(result.dump());
                return res;

            } catch (const std::exception& e) {
                spdlog::error("[API] Exception: {}", e.what());
                return crow::response(500, json{{"success", false}, {"message", e.what()}}.dump());
            } catch (...) {
                return crow::response(400, "Invalid JSON format");
            }
        });

        // ==========================================
        // API: Search 2DID (CMD 238)
        // ==========================================
        CROW_ROUTE(app, "/api/search-twodid").methods(crow::HTTPMethod::Post)
        ([service](const crow::request& req){
            try {
                auto x = json::parse(req.body);
                std::string emp = x.value("emp_no", "");
                std::string twodid = x.value("twodid", "");

                json result = service->search_2did(emp, twodid);
                
                crow::response res;
                res.code = 200;
                res.add_header("Content-Type", "application/json");
                res.write(result.dump());
                return res;
            } catch (...) {
                return crow::response(400, "Invalid JSON");
            }
        });

        // ==========================================
        // API: Write Single 2DID (CMD 239 單筆)
        // ==========================================
        CROW_ROUTE(app, "/api/write-2did").methods(crow::HTTPMethod::Post)
        ([service](const crow::request& req){
            try {
                auto x = json::parse(req.body);
                
                json result = service->process_single_upload(x);
                
                crow::response res;
                res.code = 200;
                res.add_header("Content-Type", "application/json");
                res.write(result.dump());
                return res;
            } catch (...) {
                return crow::response(400, "Invalid JSON");
            }
        });

        // ==========================================
        // API: Write Batch 2DIDs (CMD 239 批量)
        // ==========================================
        CROW_ROUTE(app, "/api/write-2dids").methods(crow::HTTPMethod::Post)
        ([service](const crow::request& req){
            try {
                auto listJson = json::parse(req.body);
                
                json result = service->process_batch_upload(listJson);
                
                crow::response res;
                res.code = 200;
                res.add_header("Content-Type", "application/json");
                res.write(result.dump());
                return res;
            } catch (...) {
                return crow::response(400, "Invalid JSON");
            }
        });

        // ==========================================
        // API: Delete WorkOrder
        // ==========================================
        CROW_ROUTE(app, "/api/delete-workorder").methods(crow::HTTPMethod::Post)
        ([service](const crow::request& req){
            try {
                auto x = json::parse(req.body);
                std::string wo = x.value("workorder", "");
                
                service->delete_work_order(wo);
                
                crow::response res;
                res.code = 200;
                res.add_header("Content-Type", "application/json");
                res.write(json{{"success", true}}.dump());
                return res;
            } catch (...) {
                return crow::response(400, "Invalid JSON");
            }
        });
    }
};
