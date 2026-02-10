#pragma once
#include <string>
#include <vector>
#include <memory>
#include <iostream>
#include <algorithm>
#include <map> // ✅ 新增 map 用於統計計數
#include <spdlog/spdlog.h> // ✅ 必須引入
#include "../core/DatabasePool.hpp"
#include "../core/AppState.hpp"
#include "../services/MesService.hpp"
#include "../models/WorkOrder.hpp"

// 輔助結構：掃描紀錄
struct ScannedData {
    std::string workOrder, sht_no, panel_no, ret_type, status;
    long long timestamp;
};

class OrderService {
    std::shared_ptr<DbPool> db_pool_;
    std::shared_ptr<MesService> mes_service_;

    // 私有輔助函式：防止 SQL Injection
    std::string sql_escape(const std::string& str) {
        std::string out;
        out.reserve(str.size() * 2);
        for (char c : str) {
            if (c == '\'') out += "\\'";
            else if (c == '\\') out += "\\\\";
            else out += c;
        }
        return out;
    }

    // 安全的字串轉整數
    int safe_stoi(const char* str, int default_val = 0) {
        if (!str) return default_val;
        try {
            return std::stoi(str);
        } catch (...) {
            return default_val;
        }
    }

    long long safe_stoll(const char* str, long long default_val = 0) {
        if (!str) return default_val;
        try {
            return std::stoll(str);
        } catch (...) {
            return default_val;
        }
    }

public:
    OrderService(std::shared_ptr<DbPool> db, std::shared_ptr<MesService> mes) 
        : db_pool_(db), mes_service_(mes) {}

    // =========================================================
    // 1. 工單查詢 (Get WorkOrder)
    // =========================================================
    json get_work_order(const std::string& wo, const std::string& emp_no, bool insert_db) {
        spdlog::info("[OrderService] Getting WorkOrder: {}", wo);

        // Step 1: 查 DB
        try {
            json dbResult = readWorkOrderFromDB(wo);
            if (!dbResult.is_null()) {
                spdlog::info("[OrderService] Found in DB");
                return {{"success", true}, {"source", "DB"}, {"data", dbResult}};
            }
        } catch (const std::exception& e) {
            spdlog::error("[OrderService] DB Read Error: {}", e.what());
        }

        // Step 2: 檢查 MES
        if (!AppState::isMesOnline.load()) {
            return {{"success", false}, {"type", "mes_offline"}, {"message", "MES Offline"}};
        }

        spdlog::info("[OrderService] Querying MES 235...");
        std::string res235 = mes_service_->send_soap_request(235, emp_no, wo);
        if (res235.empty() && !AppState::isMesOnline.load()) {
             return {{"success", false}, {"type", "mes_offline"}, {"message", "MES Timeout"}};
        }

        WorkOrderData d235 = mes_service_->parse_soap_response(res235, wo, 235);
        if (d235.valid) {
            spdlog::info("[OrderService] 235 Valid. PanelNum: {}", d235.panel_num);
            if (insert_db) saveWorkOrderToDB(d235);
            return build_wo_response(d235, "API235");
        }

        spdlog::info("[OrderService] Querying MES 236...");
        std::string res236 = mes_service_->send_soap_request(236, emp_no, wo);
        if (res236.empty() && !AppState::isMesOnline.load()) {
             return {{"success", false}, {"type", "mes_offline"}, {"message", "MES Timeout"}};
        }

        WorkOrderData d236 = mes_service_->parse_soap_response(res236, wo, 236);
        if (d236.valid) {
            spdlog::info("[OrderService] 236 Valid");
            if (insert_db) saveWorkOrderToDB(d236);
            return build_wo_response(d236, "API236");
        }

        return {{"success", false}, {"message", res236}}; 
    }

    // =========================================================
    // 2. 查詢 2DID (Search 2DID - CMD 238)
    // =========================================================
    json search_2did(const std::string& emp_no, const std::string& twodid) {
        if (!AppState::isMesOnline.load()) {
            return {{"success", false}, {"type", "mes_offline"}, {"message", "因與 IT server 網路中斷，因此 2DID 資訊查詢失敗"}};
        }

        std::string raw = mes_service_->send_soap_request(238, emp_no, twodid);

        if (raw.empty()) {
             if (!AppState::isMesOnline.load()) {
                 return {{"success", false}, {"type", "mes_offline"}, {"message", "因與 IT server 網路中斷，因此 2DID 資訊查詢失敗"}};
             }
             return {{"success", false}, {"message", "Not Found"}};
        }

        if (raw.find("OK") == 0) {
            return {{"success", true}, {"result", {{"result", raw}}}};
        }

        return {{"success", false}, {"message", "Not Found"}};
    }

    // =========================================================
    // 3. 批量上傳處理 (Process Batch Upload)
    // =========================================================
    json process_batch_upload(const json& listJson) {
        if (!listJson.is_array()) return {{"success", false}, {"message", "Invalid List"}};
        if (listJson.empty()) return {{"success", true}, {"count", 0}};

        std::vector<ScannedData> dbList;
        std::string emp = listJson[0].value("emp_no", "");

        for (const auto& x : listJson) {
            std::string wo = x.value("workOrder", "");
            std::string sht = x.value("sht_no", "");
            std::string pnl = x.value("panel_no", "");
            std::string entry = x.value("entryTime", "");
            std::string exit = x.value("exitTime", "");
            if (exit.empty()) exit = "NOW";

            std::string type = x.value("twodid_type", "Y");
            std::string remark = x.value("remark", "");
            std::string item = x.value("item", "");
            std::string step = x.value("workStep", "");

            std::string type_code = (type == "OK") ? "N" : "Y";
            
            std::string msg = wo + ";" + item + ";" + step + ";" + sht + ";" + pnl + ";" + step + ";" + entry + ";" + exit + ";" + type_code + ";" + remark + ";;";
            
            safe_upload_2did(emp, msg);

            long long ts = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            dbList.push_back({wo, sht, pnl, type, remark, ts});
        }

        saveScannedListToDB(dbList);

        return {
            {"success", true}, 
            {"mes_status", AppState::isMesOnline.load() ? "online" : "offline"}
        };
    }

    // =========================================================
    // 4. 單筆上傳處理 (Process Single Upload)
    // =========================================================
    json process_single_upload(const json& x) {
        try {
            std::string emp = x.value("emp_no", "");
            std::string wo = x.value("workOrder", "");
            std::string sht = x.value("sht_no", "");
            std::string pnl = x.value("panel_no", "");
            std::string entry = x.value("entryTime", "");
            std::string exit = x.value("exitTime", "");
            if (exit.empty()) exit = "NOW";

            std::string type = x.value("twodid_type", "Y");
            std::string remark = x.value("remark", "");
            std::string item = x.value("item", "");
            std::string step = x.value("workStep", "");

            std::string type_code = (type == "OK") ? "N" : "Y";
            
            std::string msg = wo + ";" + item + ";" + step + ";" + sht + ";" + pnl + ";" + step + ";" + entry + ";" + exit + ";" + type_code + ";" + remark + ";;";
            
            safe_upload_2did(emp, msg);

            long long ts = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            std::vector<ScannedData> dbList;
            dbList.push_back({wo, sht, pnl, type, remark, ts});
            saveScannedListToDB(dbList);

            return {
                {"success", true}, 
                {"mes_status", AppState::isMesOnline.load() ? "online" : "offline"}
            };
        } catch (const std::exception& e) {
            return {{"success", false}, {"message", "JSON Error: " + std::string(e.what())}};
        }
    }

    // =========================================================
    // 5. 刪除工單
    // =========================================================
    void delete_work_order(const std::string& wo) {
        auto con = db_pool_->getConnection();
        if(!con) return;
        std::string sql = "DELETE FROM 2DID_workorder WHERE work_order = '" + sql_escape(wo) + "'";
        mysql_query(con, sql.c_str());
        db_pool_->releaseConnection(con);
    }

private:
    void safe_upload_2did(const std::string& emp_no, const std::string& msg) {
        if (!AppState::isMesOnline.load()) {
            saveUnsentMessage(emp_no, msg);
            return;
        }

        std::string res = mes_service_->send_soap_request(239, emp_no, msg);
        if (res.empty()) {
            if (AppState::isMesOnline.load()) {
                spdlog::warn("[OrderService] Upload Failed. Switching to Offline Mode.");
                AppState::isMesOnline = false;
            }
            saveUnsentMessage(emp_no, msg);
        }
    }

    json build_wo_response(const WorkOrderData& d, const std::string& src) {
        json j;
        j["workorder"] = d.workorder;
        j["item"] = d.item;
        j["workStep"] = d.workStep;
        j["panel_num"] = d.panel_num;
        j["cmd236_flag"] = d.cmd236_flag;
        j["sht_no"] = d.sht_no;
        j["panel_no"] = d.panel_no;
        j["twodid_step"] = d.twodid_step;
        j["twodid_type"] = d.twodid_type;
        j["scanned_data"] = nullptr;
        return {{"success", true}, {"source", src}, {"data", j}};
    }

    void saveWorkOrderToDB(const WorkOrderData& d) {
        spdlog::info("[OrderService] Saving WO to DB...");
        auto con = db_pool_->getConnection();
        if(!con) return;
        
        const char* query = "INSERT INTO 2DID_workorder (work_order, product_item, work_step, panel_sum, cmd236_flag) "
                            "VALUES (?, ?, ?, ?, ?) "
                            "ON DUPLICATE KEY UPDATE product_item=?, work_step=?, panel_sum=?";
        
        MYSQL_STMT* stmt = mysql_stmt_init(con);
        if (stmt) {
            if (mysql_stmt_prepare(stmt, query, strlen(query)) == 0) {
                MYSQL_BIND bind[8];
                memset(bind, 0, sizeof(bind));
                
                unsigned long wo_len = d.workorder.length();
                unsigned long item_len = d.item.length();
                unsigned long step_len = d.workStep.length();
                int cmd_flag = d.cmd236_flag ? 1 : 0;

                bind[0].buffer_type = MYSQL_TYPE_STRING; bind[0].buffer = (char*)d.workorder.c_str(); bind[0].length = &wo_len;
                bind[1].buffer_type = MYSQL_TYPE_STRING; bind[1].buffer = (char*)d.item.c_str();      bind[1].length = &item_len;
                bind[2].buffer_type = MYSQL_TYPE_STRING; bind[2].buffer = (char*)d.workStep.c_str();  bind[2].length = &step_len;
                bind[3].buffer_type = MYSQL_TYPE_LONG;   bind[3].buffer = (char*)&d.panel_num;
                bind[4].buffer_type = MYSQL_TYPE_LONG;   bind[4].buffer = (char*)&cmd_flag;
                
                bind[5].buffer_type = MYSQL_TYPE_STRING; bind[5].buffer = (char*)d.item.c_str();      bind[5].length = &item_len;
                bind[6].buffer_type = MYSQL_TYPE_STRING; bind[6].buffer = (char*)d.workStep.c_str();  bind[6].length = &step_len;
                bind[7].buffer_type = MYSQL_TYPE_LONG;   bind[7].buffer = (char*)&d.panel_num;

                mysql_stmt_bind_param(stmt, bind);
                mysql_stmt_execute(stmt);
            }
            mysql_stmt_close(stmt);
        }

        const char* delQuery = "DELETE FROM 2DID_expected_products WHERE work_order = ?";
        stmt = mysql_stmt_init(con);
        if (stmt) {
            if (mysql_stmt_prepare(stmt, delQuery, strlen(delQuery)) == 0) {
                MYSQL_BIND bind[1];
                memset(bind, 0, sizeof(bind));
                unsigned long wo_len = d.workorder.length();
                bind[0].buffer_type = MYSQL_TYPE_STRING; bind[0].buffer = (char*)d.workorder.c_str(); bind[0].length = &wo_len;
                mysql_stmt_bind_param(stmt, bind);
                mysql_stmt_execute(stmt);
            }
            mysql_stmt_close(stmt);
        }

        if (!d.sht_no.empty()) {
            mysql_query(con, "START TRANSACTION");
            const char* insQuery = "INSERT INTO 2DID_expected_products (work_order, sheet_no, panel_no, twodid_step, twodid_type) VALUES (?, ?, ?, ?, ?)";
            stmt = mysql_stmt_init(con);
            if (stmt) {
                if (mysql_stmt_prepare(stmt, insQuery, strlen(insQuery)) == 0) {
                    MYSQL_BIND bind[5];
                    memset(bind, 0, sizeof(bind));
                    
                    unsigned long wo_len = d.workorder.length();
                    char sht_buf[100], pnl_buf[100], step_buf[100], type_buf[20];
                    unsigned long sht_len, pnl_len, step_len, type_len;

                    bind[0].buffer_type = MYSQL_TYPE_STRING; bind[0].buffer = (char*)d.workorder.c_str(); bind[0].length = &wo_len;
                    bind[1].buffer_type = MYSQL_TYPE_STRING; bind[1].buffer = sht_buf; bind[1].length = &sht_len;
                    bind[2].buffer_type = MYSQL_TYPE_STRING; bind[2].buffer = pnl_buf; bind[2].length = &pnl_len;
                    bind[3].buffer_type = MYSQL_TYPE_STRING; bind[3].buffer = step_buf; bind[3].length = &step_len;
                    bind[4].buffer_type = MYSQL_TYPE_STRING; bind[4].buffer = type_buf; bind[4].length = &type_len;

                    mysql_stmt_bind_param(stmt, bind);

                    for (size_t i = 0; i < d.sht_no.size(); ++i) {
                        // ✅ [修正] 增加安全複製與 Null 結尾
                        strncpy(sht_buf, d.sht_no[i].c_str(), sizeof(sht_buf) - 1); sht_buf[sizeof(sht_buf) - 1] = '\0';
                        sht_len = strlen(sht_buf);

                        strncpy(pnl_buf, d.panel_no[i].c_str(), sizeof(pnl_buf) - 1); pnl_buf[sizeof(pnl_buf) - 1] = '\0';
                        pnl_len = strlen(pnl_buf);

                        strncpy(step_buf, d.twodid_step[i].c_str(), sizeof(step_buf) - 1); step_buf[sizeof(step_buf) - 1] = '\0';
                        step_len = strlen(step_buf);

                        strncpy(type_buf, d.twodid_type[i].c_str(), sizeof(type_buf) - 1); type_buf[sizeof(type_buf) - 1] = '\0';
                        type_len = strlen(type_buf);
                        
                        mysql_stmt_execute(stmt);
                    }
                }
                mysql_stmt_close(stmt);
            }
            mysql_query(con, "COMMIT");
        }

        db_pool_->releaseConnection(con);
    }

    json readWorkOrderFromDB(const std::string& wo) {
        // ✅ [Debug] 開始讀取
        spdlog::debug("[DB] Read WO: Start");
        
        MYSQL* con = db_pool_->getConnection();
        if (!con) {
            spdlog::error("[DB] Failed to get connection");
            return nullptr;
        }

        json result = nullptr;
        std::string sql = "SELECT * FROM 2DID_workorder WHERE work_order = '" + sql_escape(wo) + "'";
        
        if (mysql_query(con, sql.c_str()) == 0) {
            MYSQL_RES* res = mysql_store_result(con);
            // ✅ [修正] 檢查 res 是否為 NULL
            if (res) {
                MYSQL_ROW row = mysql_fetch_row(res);
                if (row) {
                    // ✅ [Debug] 讀取主表成功
                    spdlog::debug("[DB] Main row found");
                    
                    result["workorder"] = row[0] ? row[0] : "";
                    result["item"] = row[1] ? row[1] : "";
                    result["workStep"] = row[2] ? row[2] : "";
                    result["panel_num"] = safe_stoi(row[3], 0);
                    result["cmd236_flag"] = (safe_stoi(row[4], 0) == 1);
                    mysql_free_result(res);

                    // --- 讀取 Detail ---
                    std::string detailSql = "SELECT sheet_no, panel_no, twodid_step, twodid_type FROM 2DID_expected_products WHERE work_order = '" + sql_escape(wo) + "'";
                    if (mysql_query(con, detailSql.c_str()) == 0) {
                        MYSQL_RES* resDet = mysql_store_result(con);
                        if (resDet) {
                            std::vector<std::string> sht, pnl, step, type;
                            while ((row = mysql_fetch_row(resDet))) {
                                sht.push_back(row[0] ? row[0] : "");
                                pnl.push_back(row[1] ? row[1] : "");
                                step.push_back(row[2] ? row[2] : "");
                                type.push_back(row[3] ? row[3] : "");
                            }
                            result["sht_no"] = sht; result["panel_no"] = pnl;
                            result["twodid_step"] = step; result["twodid_type"] = type;
                            mysql_free_result(resDet);
                        }
                    }

                    // --- 讀取 Scanned (使用 CTE) ---
                    // 注意：如果 MySQL 版本過低不支援 CTE，這裡會失敗
                    std::string scanSql = "WITH Ranked AS ("
                    "  SELECT sheet_no, panel_no, twodid_type, twodid_status, timestamp, "
                    "         ROW_NUMBER() OVER (PARTITION BY sheet_no ORDER BY timestamp DESC) as rn "
                    "  FROM 2DID_scanned_products "
                    "  WHERE work_order = '" + sql_escape(wo) + "' "
                    ") "
                    "SELECT sheet_no, panel_no, twodid_type, twodid_status, timestamp "
                    "FROM Ranked WHERE rn = 1 "
                    "ORDER BY timestamp ASC";
                    
                    json scannedData = json::array();
                    if (mysql_query(con, scanSql.c_str()) == 0) {
                        MYSQL_RES* resScan = mysql_store_result(con);
                        if (resScan) {
                            while ((row = mysql_fetch_row(resScan))) {
                                json item;
                                item["sheet_no"] = row[0] ? row[0] : "";
                                item["panel_no"] = row[1] ? row[1] : "";
                                item["twodid_type"] = row[2] ? row[2] : "";
                                item["twodid_status"] = row[3] ? row[3] : ""; 
                                // ✅ [修正] 使用 safe_stoll 防止崩潰
                                item["timestamp"] = safe_stoll(row[4], 0);
                                scannedData.push_back(item);
                            }
                            mysql_free_result(resScan);
                        }
                    } else {
                        spdlog::warn("[DB] Scan SQL failed: {}", mysql_error(con));
                    }
                    result["scanned_data"] = scannedData;
                } else {
                    // 查無資料
                    mysql_free_result(res);
                }
            } else {
                spdlog::error("[DB] Store Result failed: {}", mysql_error(con));
            }
        } else {
            spdlog::error("[DB] Query failed: {}", mysql_error(con));
        }

        db_pool_->releaseConnection(con);
        spdlog::debug("[DB] Read WO: End");
        return result;
    }

    void saveScannedListToDB(const std::vector<ScannedData>& list) {
        if (list.empty()) return;
        MYSQL* con = db_pool_->getConnection();
        if (!con) return;
        mysql_query(con, "START TRANSACTION");

        const char* query = "INSERT INTO 2DID_scanned_products (work_order, sheet_no, panel_no, twodid_type, twodid_status, timestamp) VALUES (?, ?, ?, ?, ?, ?)";
        MYSQL_STMT* stmt = mysql_stmt_init(con);
        if (!stmt) { db_pool_->releaseConnection(con); return; }
        if (mysql_stmt_prepare(stmt, query, strlen(query))) { mysql_stmt_close(stmt); db_pool_->releaseConnection(con); return; }

        MYSQL_BIND bind[6];
        unsigned long str_lens[5];
        char workOrder_buf[100], sheet_buf[100], panel_buf[100], type_buf[20], status_buf[200];
        long long ts_val;
        memset(bind, 0, sizeof(bind));

        bind[0].buffer_type = MYSQL_TYPE_STRING; bind[0].buffer = workOrder_buf; bind[0].length = &str_lens[0];
        bind[1].buffer_type = MYSQL_TYPE_STRING; bind[1].buffer = sheet_buf; bind[1].length = &str_lens[1];
        bind[2].buffer_type = MYSQL_TYPE_STRING; bind[2].buffer = panel_buf; bind[2].length = &str_lens[2];
        bind[3].buffer_type = MYSQL_TYPE_STRING; bind[3].buffer = type_buf; bind[3].length = &str_lens[3];
        bind[4].buffer_type = MYSQL_TYPE_STRING; bind[4].buffer = status_buf; bind[4].length = &str_lens[4];
        bind[5].buffer_type = MYSQL_TYPE_LONGLONG; bind[5].buffer = &ts_val;

        if (mysql_stmt_bind_param(stmt, bind)) {}

        // ✅ [修正] 用於統計此批次每個工單的增量 (解決批量重複工單只加 1 的問題)
        std::map<std::string, std::pair<int, int>> wo_increments; // Key: WO, Value: {OK_inc, NG_inc}

        for (const auto& d : list) {
            strncpy(workOrder_buf, d.workOrder.c_str(), sizeof(workOrder_buf)-1); workOrder_buf[sizeof(workOrder_buf)-1]='\0'; str_lens[0] = strlen(workOrder_buf);
            strncpy(sheet_buf, d.sht_no.c_str(), sizeof(sheet_buf)-1); sheet_buf[sizeof(sheet_buf)-1]='\0'; str_lens[1] = strlen(sheet_buf);
            strncpy(panel_buf, d.panel_no.c_str(), sizeof(panel_buf)-1); panel_buf[sizeof(panel_buf)-1]='\0'; str_lens[2] = strlen(panel_buf);
            strncpy(type_buf, d.ret_type.c_str(), sizeof(type_buf)-1); type_buf[sizeof(type_buf)-1]='\0'; str_lens[3] = strlen(type_buf);
            strncpy(status_buf, d.status.c_str(), sizeof(status_buf)-1); status_buf[sizeof(status_buf)-1]='\0'; str_lens[4] = strlen(status_buf);
            ts_val = d.timestamp;
            mysql_stmt_execute(stmt);

            // 統計
            if (d.ret_type == "OK") wo_increments[d.workOrder].first++;
            else wo_increments[d.workOrder].second++;
        }
        mysql_stmt_close(stmt);

        // ✅ [修正] 針對每個工單執行精確的 Update
        for (const auto& kv : wo_increments) {
            std::string wo = kv.first;
            int ok_inc = kv.second.first;
            int ng_inc = kv.second.second;
            
            if (ok_inc > 0 || ng_inc > 0) {
                std::string updateSql = "UPDATE 2DID_workorder SET OK_sum = OK_sum + " + std::to_string(ok_inc) + 
                                        ", NG_sum = NG_sum + " + std::to_string(ng_inc) + 
                                        " WHERE work_order = '" + sql_escape(wo) + "'";
                mysql_query(con, updateSql.c_str());
            }
        }

        mysql_query(con, "COMMIT");
        db_pool_->releaseConnection(con);
    }

    void saveUnsentMessage(const std::string& emp, const std::string& msg) {
        auto con = db_pool_->getConnection();
        if(!con) return;
        
        const char* query = "INSERT INTO 2DID_unsent_messages (emp_no, message) VALUES (?, ?)";
        MYSQL_STMT* stmt = mysql_stmt_init(con);
        if (!stmt) { db_pool_->releaseConnection(con); return; }
        
        if (mysql_stmt_prepare(stmt, query, strlen(query))) {
            mysql_stmt_close(stmt);
            db_pool_->releaseConnection(con);
            return;
        }

        MYSQL_BIND bind[2];
        memset(bind, 0, sizeof(bind));
        unsigned long emp_len = emp.length();
        unsigned long msg_len = msg.length();

        bind[0].buffer_type = MYSQL_TYPE_STRING; bind[0].buffer = (char*)emp.c_str(); bind[0].length = &emp_len;
        bind[1].buffer_type = MYSQL_TYPE_STRING; bind[1].buffer = (char*)msg.c_str(); bind[1].length = &msg_len;

        if (!mysql_stmt_bind_param(stmt, bind)) {
            mysql_stmt_execute(stmt);
        }

        mysql_stmt_close(stmt);
        db_pool_->releaseConnection(con);
    }
};