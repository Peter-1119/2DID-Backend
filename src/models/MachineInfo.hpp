#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "../core/Config.hpp"

using json = nlohmann::json;

struct MachineInfo {
    std::string machine_id;
    std::string plc_ip;
    int plc_port;
    std::string plc_type;
    int addr_trigger;
    int addr_result;
    
    // 詳細點位 (從 DB metadata 解析)
    int addr_up_in = PlcDefaultPoints::UP_IN;
    int addr_up_out = PlcDefaultPoints::UP_OUT;
    int addr_dn_in = PlcDefaultPoints::DN_IN;
    int addr_dn_out = PlcDefaultPoints::DN_OUT;
    int addr_start = PlcDefaultPoints::START;

    // 相機 IP (從 CMD 254 取得)
    std::string cam_left1_ip;
    std::string cam_left2_ip;
    std::string cam_right1_ip;
    std::string cam_right2_ip;

    static MachineInfo from_db_row(const std::string& id, const std::string& ip, int port, const std::string& type, int trig, int res, const std::string& metadata_json) {
        MachineInfo info;
        info.machine_id = id;
        info.plc_ip = ip;
        info.plc_port = port;
        info.plc_type = type;
        info.addr_trigger = trig;
        info.addr_result = res;
        
        if (!metadata_json.empty()) {
            try {
                auto j = json::parse(metadata_json);
                if (j.contains("addr_up_in")) info.addr_up_in = j["addr_up_in"];
                if (j.contains("addr_up_out")) info.addr_up_out = j["addr_up_out"];
                if (j.contains("addr_dn_in")) info.addr_dn_in = j["addr_dn_in"];
                if (j.contains("addr_dn_out")) info.addr_dn_out = j["addr_dn_out"];
                if (j.contains("addr_start")) info.addr_start = j["addr_start"];
            } catch (...) {}
        }
        return info;
    }
};