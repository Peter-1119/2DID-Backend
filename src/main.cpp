#include "crow_all.h"
#include "core/DatabasePool.hpp"
#include "core/ThreadPool.hpp"
#include "core/AppState.hpp"
#include "core/Config.hpp"
#include "managers/PlcConnectionManager.hpp"
#include "services/PlcService.hpp"
#include "services/MesService.hpp"
#include "controllers/PlcController.hpp"
#include "controllers/SystemController.hpp"
#include "services/OrderService.hpp"
#include "controllers/OrderController.hpp"
#include <mysql.h> // 確保有 include 這個

// 全域物件
std::shared_ptr<ThreadPool> g_workerPool;
std::shared_ptr<DbPool> g_dbPool;

// 背景監控執行緒 (完整還原 BackendService 邏輯)
void MonitorLoop(std::shared_ptr<MesService> mesService) {
    mysql_thread_init();
    spdlog::info("[Monitor] Thread Started");
    
    while (true) {
        try {
            if (!AppState::isMesOnline) {
                // 離線模式：Ping Server
                if (mesService->ping_server()) {
                    spdlog::info("[System] MES Server is Back Online!");
                    AppState::isMesOnline = true;
                }
            } else {
                // 線上模式：檢查積壓的 unsent messages
                MYSQL* con = g_dbPool->getConnection();
                if (con) {
                    std::string sql = "SELECT id, emp_no, message FROM 2DID_unsent_messages ORDER BY id ASC LIMIT 10";
                    if (mysql_query(con, sql.c_str()) == 0) {
                        MYSQL_RES* res = mysql_store_result(con);
                        if (res) {
                            MYSQL_ROW row;
                            std::vector<long long> processed_ids;
                            while ((row = mysql_fetch_row(res))) {
                                long long id = std::stoll(row[0]);
                                std::string ret = mesService->send_soap_request(239, row[1], row[2]);
                                if (ret.empty()) { // 再次失敗
                                    AppState::isMesOnline = false;
                                    break; 
                                }
                                processed_ids.push_back(id);
                            }
                            mysql_free_result(res);

                            if (!processed_ids.empty()) {
                                std::string delSql = "DELETE FROM 2DID_unsent_messages WHERE id IN (";
                                for (size_t i = 0; i < processed_ids.size(); ++i) {
                                    if (i > 0) delSql += ",";
                                    delSql += std::to_string(processed_ids[i]);
                                }
                                delSql += ")";
                                mysql_query(con, delSql.c_str());
                                spdlog::info("[System] Resent {} buffered messages.", processed_ids.size());
                            }
                        }
                    }
                    g_dbPool->releaseConnection(con);
                }
            }
        } catch (const std::exception& e) {
            spdlog::error("[Monitor] Exception: {}", e.what());
        } catch (...) {
            spdlog::error("[Monitor] Unknown Exception");
        }
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    mysql_thread_end();
}

int main() {
    // 1. 初始化環境
    spdlog::flush_on(spdlog::level::info);
    if (curl_global_init(CURL_GLOBAL_ALL) != 0) { spdlog::error("CURL Init Failed"); return 1; }
    if (mysql_library_init(0, NULL, NULL)) { spdlog::error("MySQL Lib Init Failed"); return 1; }
    spdlog::info("[System] Libraries Initialized");
    
    // 2. 初始化 DB Pool
    g_workerPool = std::make_shared<ThreadPool>(4);
    g_dbPool = std::make_shared<DbPool>(CFG_DB_HOST, CFG_DB_PORT, CFG_DB_USER, CFG_DB_PASS, CFG_DB_NAME);
    
    // 3. 初始化 Services
    boost::asio::io_context ioc;
    auto work_guard = boost::asio::make_work_guard(ioc);
    auto mesService = std::make_shared<MesService>();
    auto plcManager = std::make_shared<PlcConnectionManager>(ioc);
    auto plcService = std::make_shared<PlcService>(g_dbPool, plcManager);
    auto orderService = std::make_shared<OrderService>(g_dbPool, mesService);

    std::thread monitorThread([mesService](){ MonitorLoop(mesService); });
    monitorThread.detach();
    std::thread io_thread([&ioc](){ ioc.run(); });

    crow::App<crow::CORSHandler> app;
    PlcController::init(app, plcService);
    SystemController::init(app, mesService);
    OrderController::init(app, orderService);

    // ✅ Crow 會自動開啟多執行緒處理 Request
    app.port(2151).multithreaded().run();

    // 清理
    ioc.stop();
    io_thread.join();
    
    mysql_library_end();
    curl_global_cleanup();
    return 0;
}