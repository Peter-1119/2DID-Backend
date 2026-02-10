#pragma once
#include <string>
#include <vector>
#include <sstream>
#include <set>
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include "../core/Config.hpp"
#include "../models/WorkOrder.hpp" // 需引用 WorkOrder 結構

using json = nlohmann::json;

class MesService {
public:
    // IIS 驗證 (Blocking)
    std::string validate_employee_iis(const std::string& empId) {
        json innerJson; innerJson["Emp_NO"] = empId;
        cpr::Response r = cpr::Post(cpr::Url{IIS_API_URL},
            cpr::Payload{ {"CmdCode", "5"}, {"InMessage_Json", innerJson.dump()} },
            cpr::Timeout{3000});

        if (r.status_code == 200) return r.text;
        spdlog::error("[IIS] Validate Emp Failed: {}", r.error.message);
        return "";
    }

    // Ping (Blocking)
    bool ping_server() {
        cpr::Response r = cpr::Get(cpr::Url{SOAP_URL}, cpr::Timeout{500});
        return (r.status_code == 200);
    }

    // SOAP 請求
    std::string send_soap_request(int command, const std::string& emp_no, const std::string& message) {
        // 每個執行緒保留一個 session 實體，實現真正的 Keep-Alive 重用
        static thread_local cpr::Session session;
        session.SetUrl(cpr::Url{SOAP_URL});
        session.SetHeader(cpr::Header{{"Content-Type", "text/xml;charset=utf-8"}, {"SOAPAction", SOAP_ACTION}, {"Connection", "keep-alive"}});
        session.SetTimeout(cpr::Timeout{3000});
        
        std::string xml = build_xml(command, emp_no, message);
        session.SetBody(cpr::Body{xml});
        cpr::Response r = session.Post();

        if (r.status_code == 200) {
            std::string target = "<UpLoadImageResult>";
            std::string end_target = "</UpLoadImageResult>";
            size_t start = r.text.find(target);
            size_t end = r.text.find(end_target);
            if (start != std::string::npos && end != std::string::npos) {
                return r.text.substr(start + target.length(), end - start - target.length());
            }
        }
        return "";
    }

    // ✅ [新增] 解析 SOAP 回傳的工單資料 (移植自 BackendService.cpp)
    WorkOrderData parse_soap_response(std::string raw, std::string inputWO, int cmdType) {
        WorkOrderData data;
        if (raw.find("OK") != 0) return data;
        
        std::stringstream ss(raw);
        std::string line;
        std::vector<std::string> lines;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) lines.push_back(line);
        }
        if (lines.empty()) return data;

        data.workorder = inputWO;
        data.cmd236_flag = (cmdType == 236);

        for (size_t i = 0; i < lines.size(); ++i) {
            std::stringstream lineSS(lines[i]);
            std::string segment;
            std::vector<std::string> parts;
            while(std::getline(lineSS, segment, ';')) parts.push_back(segment);
            
            int offset = (i == 0) ? 1 : 0;
            if (i == 0) {
                if (cmdType == 235 && parts.size() > (1 + offset)) {
                    data.item = parts[0 + offset];
                    data.workStep = parts[1 + offset];
                } else if (cmdType == 236 && parts.size() > (2 + offset)) {
                    data.item = parts[1 + offset];
                    data.workStep = parts[2 + offset];
                }
            }
            if (cmdType == 235 && parts.size() >= (6 + offset)) {
                data.sht_no.push_back(parts[2 + offset]);
                data.panel_no.push_back(parts[3 + offset]);
                data.twodid_step.push_back(parts[4 + offset]);
                data.twodid_type.push_back(parts[5 + offset]);
            } else if (cmdType == 236 && parts.size() > (8 + offset)) {
                data.sht_no.push_back(parts[3 + offset]);
                data.panel_no.push_back(parts[4 + offset]);
                data.twodid_step.push_back(parts[5 + offset]);
                data.twodid_type.push_back(parts[6 + offset]);
                data.panel_num = std::stoi(parts[8 + offset].substr(1));
            }
        }
        if (cmdType == 235) {
            std::set<std::string> unique_panels(data.panel_no.begin(), data.panel_no.end());
            data.panel_num = unique_panels.size();
        }
        data.valid = true;
        return data;
    }

    std::unordered_map<std::string, std::string> get_machine_cameras(const std::string& emp_no, const std::string& machine_id) {
        std::string raw = send_soap_request(254, emp_no, machine_id);
        std::unordered_map<std::string, std::string> result;
        
        if (raw.find("OK;") == 0) {
            // OK;10.1.206.145;10.1.207.14 10.1.207.15;10.1.206.58
            std::vector<std::string> parts;
            std::stringstream ss(raw);
            std::string segment;
            while (std::getline(ss, segment, ';')) parts.push_back(segment);

            if (parts.size() >= 4) {
                // PLC IP (parts[1]) - 這裡不一定要用，因為我們已有 DB 資料
                result["plc_ip"] = parts[1];

                // 解析左邊相機 (parts[2])，空白分隔
                std::stringstream ssl(parts[2]);
                std::string cam;
                int idx = 1;
                while (ssl >> cam) {
                    result["cam_left" + std::to_string(idx++)] = cam;
                }

                // 解析右邊相機 (parts[3])
                std::stringstream ssr(parts[3]);
                idx = 1;
                while (ssr >> cam) {
                    result["cam_right" + std::to_string(idx++)] = cam;
                }
            }
        }
        return result;
    }

private:
    std::string build_xml(int command, const std::string& emp, const std::string& msg) {
        return R"(<soapenv:Envelope xmlns:soapenv="http://schemas.xmlsoap.org/soap/envelope/" xmlns:tem="http://tempuri.org/"><soapenv:Header/><soapenv:Body><tem:UpLoadImage><tem:command>)" 
               + std::to_string(command) + R"(</tem:command><tem:emp_no>)" + emp + R"(</tem:emp_no><tem:message>)" 
               + msg + R"(</tem:message><tem:image_byte>null</tem:image_byte><tem:result></tem:result></tem:UpLoadImage></soapenv:Body></soapenv:Envelope>)";
    }
};