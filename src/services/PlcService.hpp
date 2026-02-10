#pragma once
#include <string>
#include <unordered_map>
#include "../core/DatabasePool.hpp"
#include "../managers/PlcConnectionManager.hpp"
#include "../models/MachineInfo.hpp"

class PlcService {
    std::shared_ptr<DbPool> db_pool_;
    std::shared_ptr<PlcConnectionManager> manager_;
    std::unordered_map<std::string, MachineInfo> cache_;
    std::mutex cache_mutex_;

public:
    PlcService(std::shared_ptr<DbPool> db, std::shared_ptr<PlcConnectionManager> mgr) 
        : db_pool_(db), manager_(mgr) {}

    void execute_write_status(const std::string& machine_id, const std::string& status, std::function<void(std::string)> callback) {
        MachineInfo info;
        bool found = false;

        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            if (cache_.count(machine_id)) { info = cache_[machine_id]; found = true; }
        }

        if (!found) {
            auto conn = db_pool_->getConnection();
            if (!conn) { callback("DB Error"); return; }
            
            // 根據 SQL schema 查詢
            std::string sql = "SELECT plc_ip, plc_port, plc_type, addr_write_trigger, addr_write_result, metadata FROM 2did_machine_info WHERE machine_id = '" + machine_id + "'";
            if (mysql_query(conn, sql.c_str()) == 0) {
                auto res = mysql_store_result(conn);
                if (auto row = mysql_fetch_row(res)) {
                    std::string meta = row[5] ? row[5] : "";
                    info = MachineInfo::from_db_row(
                        machine_id, 
                        row[0], 
                        std::stoi(row[1]), 
                        row[2] ? row[2] : "Mitsubishi",
                        row[3] ? std::stoi(row[3]) : 86,
                        row[4] ? std::stoi(row[4]) : 87,
                        meta
                    );
                    
                    {
                        std::lock_guard<std::mutex> lock(cache_mutex_);
                        cache_[machine_id] = info;
                    }
                    found = true;
                }
                mysql_free_result(res);
            }
            db_pool_->releaseConnection(conn);
        }

        if (!found) { callback("Machine Not Found in DB"); return; }

        bool is_ok = (status == "OK");
        manager_->write_status(info.plc_ip, info.plc_port, info.plc_type, info.addr_trigger, info.addr_result, is_ok, callback);
    }
};