// BackendService.cpp
#define WIN32_LEAN_AND_MEAN 
#define NOMINMAX 

#include "crow_all.h" 
#include <mysql.h>    
#include <nlohmann/json.hpp>
#include <cpr/cpr.h>
#include <iomanip>

#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <memory>
#include <mutex>
#include <queue>
#include <future>
#include <chrono>
#include <atomic> 
#include <thread> 

using json = nlohmann::json;
using namespace std;

// --- 配置區 ---
const char* DB_HOST = "10.8.32.64";
const int   DB_PORT = 3306;
const char* DB_USER = "sfuser";
const char* DB_PASS = "1q2w3e4R"; 
const char* DB_NAME = "sfdb4070"; 

const string SOAP_URL = "http://10.8.1.124/MESConnect.svc";
const string SOAP_ACTION = "http://tempuri.org/IMESConnect/UpLoadImage";

// ✅ [Req 2] 全域變數：MES 連線狀態
std::atomic<bool> g_isMesOnline{true};

// --- 資料結構 ---
struct WorkOrderData {
    string workorder, item, workStep;
    int panel_num = 0;
    vector<string> sht_no, panel_no, twodid_step, twodid_type;
    bool valid = false;
};

struct ScannedData {
    string workOrder, sht_no, panel_no, ret_type, status;
    long long timestamp;
};

// --- MySQL 連線池 (保持不變) ---
class DbPool {
    struct PooledConn {
        MYSQL* con;
        std::chrono::steady_clock::time_point last_used;
    };
    string host, user, pass, db;
    int port;
    queue<PooledConn> pool;
    mutex m_mutex;

public:
    DbPool(string h, int p, string u, string pwd, string d) 
        : host(h), port(p), user(u), pass(pwd), db(d) {
        for (int i = 0; i < 10; ++i) {
            MYSQL* con = createConnection();
            if (con) pool.push({con, std::chrono::steady_clock::now()});
        }
    }
    ~DbPool() {
        lock_guard<mutex> lock(m_mutex);
        while(!pool.empty()) {
            MYSQL* con = pool.front().con;
            pool.pop();
            mysql_close(con);
        }
    }
    MYSQL* createConnection() {
        MYSQL* con = mysql_init(NULL);
        if (con == NULL) return nullptr;
        int timeout = 3;
        mysql_options(con, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);

        // 關閉 SSL 驗證 (解決 0x800B0109)
        my_bool ssl_verify = 0; 
        mysql_options(con, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &ssl_verify);
        if (mysql_real_connect(con, host.c_str(), user.c_str(), pass.c_str(), db.c_str(), port, NULL, 0) == NULL) {
            mysql_close(con);
            return nullptr;
        }
        return con;
    }
    MYSQL* getConnection() {
        lock_guard<mutex> lock(m_mutex);
        if (pool.empty()) return createConnection();
        PooledConn pConn = pool.front();
        pool.pop();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - pConn.last_used).count() > 30) {
            if (mysql_ping(pConn.con) != 0) {
                mysql_close(pConn.con);
                return createConnection();
            }
        }
        return pConn.con;
    }
    void releaseConnection(MYSQL* con) {
        if (!con) return;
        lock_guard<mutex> lock(m_mutex);
        pool.push({con, std::chrono::steady_clock::now()});
    }
};

shared_ptr<DbPool> dbPool;

// --- Helper: SQL Escape ---
string sql_escape(const string& str) {
    string out;
    out.reserve(str.size());
    for (char c : str) {
        if (c == '\'') out += "\\'";
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    return out;
}

string getCurrentDateTimeStr() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    #ifdef _WIN32
        localtime_s(&tm_buf, &t);
    #else
        localtime_r(&t, &tm_buf);
    #endif
    stringstream ss;
    ss << put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

// --- SOAP Client ---
class SoapClient {
public:
    static string buildXml(int command, const string& emp_no, const string& message) {
        return R"(<soapenv:Envelope xmlns:soapenv="http://schemas.xmlsoap.org/soap/envelope/" xmlns:tem="http://tempuri.org/"><soapenv:Header/><soapenv:Body><tem:UpLoadImage><tem:command>)" 
               + to_string(command) + R"(</tem:command><tem:emp_no>)" + emp_no + R"(</tem:emp_no><tem:message>)" 
               + message + R"(</tem:message><tem:image_byte>null</tem:image_byte><tem:result></tem:result></tem:UpLoadImage></soapenv:Body></soapenv:Envelope>)";
    }

    // 標準發送 (Timeout 3s)
    static string sendRequest(int command, const string& emp_no, const string& message) {
        cpr::Response r = cpr::Post(
            cpr::Url{SOAP_URL},
            cpr::Body{buildXml(command, emp_no, message)},
            cpr::Header{
                {"Content-Type", "text/xml;charset=utf-8"},
                {"SOAPAction", SOAP_ACTION},
                {"Connection", "keep-alive"} 
            },
            cpr::Timeout{1000} // 一般請求 3秒超時
        );

        if (r.error.code != cpr::ErrorCode::OK || r.status_code != 200) {
            // 連線失敗或伺服器錯誤
            return ""; 
        }

        string target = "<UpLoadImageResult>";
        string end_target = "</UpLoadImageResult>";
        size_t start_pos = r.text.find(target);
        size_t end_pos = r.text.find(end_target);
        if (start_pos != string::npos && end_pos != string::npos) {
            return r.text.substr(start_pos + target.length(), end_pos - start_pos - target.length());
        }
        return "";
    }

    // ✅ [Req 2.1] 輕量級 Health Check (Timeout 0.5s)
    static bool pingServer() {
        // 使用 GET 請求測試 .svc 服務是否活著，這比發送完整的 XML 快得多
        cpr::Response r = cpr::Get(
            cpr::Url{SOAP_URL}, 
            cpr::Timeout{500} // 0.5 秒超時
        );
        return (r.status_code == 200);
    }
};

// --- Parse SOAP Response (保持不變) ---
WorkOrderData parseSoapResponse(string raw, string inputWO, int cmdType) {
    WorkOrderData data;
    if (raw.find("OK") != 0) return data;
    stringstream ss(raw);
    string line;
    vector<string> lines;
    while (getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    if (lines.empty()) return data;
    data.workorder = inputWO;
    data.panel_num = lines.size(); 
    for (size_t i = 0; i < lines.size(); ++i) {
        stringstream lineSS(lines[i]);
        string segment;
        vector<string> parts;
        while(getline(lineSS, segment, ';')) parts.push_back(segment);
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
        } else if (cmdType == 236 && parts.size() >= (7 + offset)) {
            data.sht_no.push_back(parts[3 + offset]); 
            data.panel_no.push_back(parts[4 + offset]);
            data.twodid_step.push_back(parts[5 + offset]); 
            data.twodid_type.push_back(parts[6 + offset]);
        }
    }
    data.valid = true;
    return data;
}

// --- DB Helper Functions (保持不變) ---
void saveWorkOrderToDB(const WorkOrderData& d) {
    MYSQL* con = dbPool->getConnection();
    if (!con) return;
    mysql_query(con, "START TRANSACTION");
    string sql = "INSERT INTO 2DID_workorder (work_order, product_item, work_step, panel_sum) VALUES ('" + 
                 sql_escape(d.workorder) + "','" + sql_escape(d.item) + "','" + sql_escape(d.workStep) + "'," + to_string(d.panel_num) + 
                 ") ON DUPLICATE KEY UPDATE product_item='" + sql_escape(d.item) + "', work_step='" + sql_escape(d.workStep) + "', panel_sum=" + to_string(d.panel_num);
    mysql_query(con, sql.c_str());
    string delSql = "DELETE FROM 2DID_expected_products WHERE work_order = '" + sql_escape(d.workorder) + "'";
    mysql_query(con, delSql.c_str());
    if (!d.sht_no.empty()) {
        string batchSql = "INSERT INTO 2DID_expected_products (work_order, sheet_no, panel_no, twodid_step, twodid_type) VALUES ";
        for (size_t i = 0; i < d.sht_no.size(); ++i) {
            if (i > 0) batchSql += ",";
            batchSql += "('" + sql_escape(d.workorder) + "','" + sql_escape(d.sht_no[i]) + "','" + sql_escape(d.panel_no[i]) + "','" + 
                        sql_escape(d.twodid_step[i]) + "','" + sql_escape(d.twodid_type[i]) + "')";
        }
        mysql_query(con, batchSql.c_str());
    }
    mysql_query(con, "COMMIT");
    dbPool->releaseConnection(con);
}

json readWorkOrderFromDB(string wo) {
    MYSQL* con = dbPool->getConnection();
    if (!con) return nullptr;
    json result = nullptr;
    string sql = "SELECT * FROM 2DID_workorder WHERE work_order = '" + sql_escape(wo) + "'";
    if (mysql_query(con, sql.c_str()) == 0) {
        MYSQL_RES* res = mysql_store_result(con);
        if (res) {
            MYSQL_ROW row = mysql_fetch_row(res);
            if (row) {
                result["workorder"] = row[0] ? row[0] : "";
                result["item"] = row[1] ? row[1] : "";
                result["workStep"] = row[2] ? row[2] : "";
                result["panel_num"] = row[3] ? stoi(row[3]) : 0;
                mysql_free_result(res); 

                string detailSql = "SELECT sheet_no, panel_no, twodid_step, twodid_type FROM 2DID_expected_products WHERE work_order = '" + sql_escape(wo) + "'";
                if (mysql_query(con, detailSql.c_str()) == 0) {
                    MYSQL_RES* resDet = mysql_store_result(con);
                    vector<string> sht, pnl, step, type;
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

                string scanSql = "WITH Ranked AS ("
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
                    while ((row = mysql_fetch_row(resScan))) {
                        json item;
                        item["sheet_no"] = row[0] ? row[0] : "";
                        item["panel_no"] = row[1] ? row[1] : "";
                        item["twodid_type"] = row[2] ? row[2] : "";
                        item["twodid_status"] = row[3] ? row[3] : ""; 
                        item["timestamp"] = row[4] ? std::stoll(row[4]) : 0;
                        scannedData.push_back(item);
                    }
                    mysql_free_result(resScan);
                }
                result["scanned_data"] = scannedData;
            } else {
                mysql_free_result(res);
            }
        }
    }
    dbPool->releaseConnection(con);
    return result;
}

void saveScannedListToDB(const vector<ScannedData>& list) {
    if (list.empty()) return;
    MYSQL* con = dbPool->getConnection();
    if (!con) return;
    mysql_query(con, "START TRANSACTION");

    const char* query = "INSERT INTO 2DID_scanned_products (work_order, sheet_no, panel_no, twodid_type, twodid_status, timestamp) VALUES (?, ?, ?, ?, ?, ?)";
    MYSQL_STMT* stmt = mysql_stmt_init(con);
    if (!stmt) { dbPool->releaseConnection(con); return; }
    if (mysql_stmt_prepare(stmt, query, strlen(query))) { mysql_stmt_close(stmt); dbPool->releaseConnection(con); return; }

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

    for (const auto& d : list) {
        strncpy(workOrder_buf, d.workOrder.c_str(), sizeof(workOrder_buf)); str_lens[0] = d.workOrder.length();
        strncpy(sheet_buf, d.sht_no.c_str(), sizeof(sheet_buf)); str_lens[1] = d.sht_no.length();
        strncpy(panel_buf, d.panel_no.c_str(), sizeof(panel_buf)); str_lens[2] = d.panel_no.length();
        strncpy(type_buf, d.ret_type.c_str(), sizeof(type_buf)); str_lens[3] = d.ret_type.length();
        strncpy(status_buf, d.status.c_str(), sizeof(status_buf)); str_lens[4] = d.status.length();
        ts_val = d.timestamp;
        mysql_stmt_execute(stmt);
    }

    string updateOK = "UPDATE 2DID_workorder SET OK_sum = OK_sum + 1 WHERE work_order IN (";
    string updateNG = "UPDATE 2DID_workorder SET NG_sum = NG_sum + 1 WHERE work_order IN (";
    bool hasOK = false, hasNG = false;
    for (const auto& d : list) {
        if (d.ret_type == "OK") { if (hasOK) updateOK += ","; updateOK += "'" + sql_escape(d.workOrder) + "'"; hasOK = true; } 
        else { if (hasNG) updateNG += ","; updateNG += "'" + sql_escape(d.workOrder) + "'"; hasNG = true; }
    }
    if (hasOK) { updateOK += ")"; mysql_query(con, updateOK.c_str()); }
    if (hasNG) { updateNG += ")"; mysql_query(con, updateNG.c_str()); }

    mysql_stmt_close(stmt);
    mysql_query(con, "COMMIT");
    dbPool->releaseConnection(con);
}

// ✅ [Req 3.1] 儲存發送失敗的訊息到 MySQL
void saveUnsentMessage(const string& emp, const string& msg) {
    MYSQL* con = dbPool->getConnection();
    if (!con) return;
    
    string sql = "INSERT INTO 2DID_unsent_messages (emp_no, message) VALUES ('" 
               + sql_escape(emp) + "', '" + sql_escape(msg) + "')";
    
    if (mysql_query(con, sql.c_str())) {
        cerr << "[DB] Save Unsent Message Failed: " << mysql_error(con) << endl;
    } 
    dbPool->releaseConnection(con);
}

// ✅ [Req 3] 安全上傳函式：封裝了「嘗試傳送 -> 失敗存 DB」的邏輯
// 這會被 write2did 與 write2dids 共用
void SafeSoapCall(string emp, string msg) {
    // [Req 3.2] 如果已知斷線，直接存 DB，不浪費時間連線
    if (!g_isMesOnline) {
        saveUnsentMessage(emp, msg);
        return;
    }

    // 嘗試發送 (使用標準 3s timeout)
    string res = SoapClient::sendRequest(239, emp, msg);

    // [Req 3.1] 如果回傳空字串 (代表連線失敗)，則轉存 DB
    if (res.empty()) {
        if (g_isMesOnline) {
            cout << "[MES] Connection Lost. Switching to Offline Mode." << endl;
            g_isMesOnline = false; // 標記為離線
        }
        saveUnsentMessage(emp, msg);
    }
}

// ✅ [Req 2] 背景監控與補上傳任務
void MonitorLoop() {
    cout << "[System] MES Monitor Thread Started." << endl;
    while (true) {
        if (!g_isMesOnline) {
            // --- [Req 2.1] 離線模式: 每 5 秒 Ping 一次 (Timeout 0.5s) ---
            bool alive = SoapClient::pingServer();
            
            if (alive) {
                cout << "[System] MES Server is Back Online!" << endl;
                g_isMesOnline = true; // 切換回線上模式，下次迴圈會進入下方處理積壓資料
            } else {
                std::this_thread::sleep_for(std::chrono::seconds(5)); // 持續等待
            }
        } 
        else {
            // --- [Req 2.2] 線上模式: 檢查積壓數據並上傳 ---
            MYSQL* con = dbPool->getConnection();
            if (!con) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                continue;
            }

            // 取出最舊的一批資料 (Batch 10)
            string sql = "SELECT id, emp_no, message FROM 2DID_unsent_messages ORDER BY id ASC LIMIT 10";
            if (mysql_query(con, sql.c_str()) == 0) {
                MYSQL_RES* res = mysql_store_result(con);
                if (res) {
                    MYSQL_ROW row;
                    vector<long long> processed_ids;
                    
                    while ((row = mysql_fetch_row(res))) {
                        long long id = stoll(row[0]);
                        string emp = row[1];
                        string msg = row[2];

                        // 嘗試補送
                        string ret = SoapClient::sendRequest(239, emp, msg);

                        if (ret.empty()) {
                            // 發送失敗，代表又斷線了
                            g_isMesOnline = false;
                            cout << "[System] Connection lost during buffered upload." << endl;
                            break; // 跳出迴圈，下次會進入離線 Ping 模式
                        } else {
                            processed_ids.push_back(id);
                        }
                    }
                    mysql_free_result(res);

                    // 批量刪除已處理的資料
                    if (!processed_ids.empty()) {
                        string delSql = "DELETE FROM 2DID_unsent_messages WHERE id IN (";
                        for (size_t i = 0; i < processed_ids.size(); ++i) {
                            if (i > 0) delSql += ",";
                            delSql += to_string(processed_ids[i]);
                        }
                        delSql += ")";
                        mysql_query(con, delSql.c_str());
                        cout << "[System] Resent " << processed_ids.size() << " buffered messages." << endl;
                    } 
                    else {
                        // 沒有資料需要處理，休息久一點
                        std::this_thread::sleep_for(std::chrono::seconds(5));
                    }
                }
            }
            dbPool->releaseConnection(con);
            
            // 避免 DB 忙碌迴圈，稍微休息
            if (g_isMesOnline) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
}

// CORS Middleware (保持不變)
struct CORSHandler {
    struct context {};
    void before_handle(crow::request& req, crow::response& res, context& ctx) {}
    void after_handle(crow::request& req, crow::response& res, context& ctx) {
        res.add_header("Access-Control-Allow-Origin", "*");
        res.add_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        res.add_header("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Requested-With");
        if (req.method == crow::HTTPMethod::Options) { res.code = 204; res.end(); return; }
    }
};

// ✅ [Req 1.1] 修改 Logger 以過濾 Heartbeat
class CustomLogger : public crow::ILogHandler {
public:
    void log(const std::string& message, crow::LogLevel level) override {
        // 如果訊息包含 "/heartbeat"，則直接忽略，不印出
        if (message.find("/heartbeat") != std::string::npos) {
            return;
        }

        string levelStr;
        switch (level) {
            case crow::LogLevel::Debug:   levelStr = "DEBUG"; break;
            case crow::LogLevel::Info:    levelStr = "INFO "; break;
            case crow::LogLevel::Warning: levelStr = "WARN "; break;
            case crow::LogLevel::Error:   levelStr = "ERROR"; break;
            case crow::LogLevel::Critical:levelStr = "CRIT "; break;
        }
        std::cout << "(" << getCurrentDateTimeStr() << ") [" << levelStr << "] " << message << std::endl;
    }
};

int main() {
    static CustomLogger logger;
    crow::logger::setHandler(&logger);
    dbPool = make_shared<DbPool>(DB_HOST, DB_PORT, DB_USER, DB_PASS, DB_NAME);
    crow::logger::setLogLevel(crow::LogLevel::Info);

    // 啟動背景監控執行緒
    std::thread monitorThread(MonitorLoop);
    monitorThread.detach(); 

    crow::App<CORSHandler> app;

    // ✅ [Req 1] API: Heartbeat 
    // 前端每秒呼叫此 API，確認後端活著。Logger 已設定不顯示此紀錄。
    CROW_ROUTE(app, "/heartbeat").methods(crow::HTTPMethod::Get)
    ([](){
        return crow::response("OK");
    });

    // API 1: Write DB (保持不變)
    CROW_ROUTE(app, "/write_to_database").methods(crow::HTTPMethod::Post)
    ([](const crow::request& req){
        auto x = json::parse(req.body);
        WorkOrderData d;
        d.workorder = x.value("workorder", "");
        d.item = x.value("item", "");
        d.workStep = x.value("workStep", "");
        d.panel_num = x.value("panel_num", 0);
        if (x.contains("sht_no")) d.sht_no = x["sht_no"].get<vector<string>>();
        if (x.contains("panel_no")) d.panel_no = x["panel_no"].get<vector<string>>();
        if (x.contains("twodid_step")) d.twodid_step = x["twodid_step"].get<vector<string>>();
        if (x.contains("twodid_type")) d.twodid_type = x["twodid_type"].get<vector<string>>();
        saveWorkOrderToDB(d);
        return crow::response(json{{"success", true}}.dump());
    });

    // API 2: Read WorkOrder
    CROW_ROUTE(app, "/api/workorder").methods(crow::HTTPMethod::Post)
    ([](const crow::request& req){
        auto x = json::parse(req.body);
        string wo = x.value("workorder", "");
        string emp = x.value("emp_no", "");
        bool insertDB = x.value("insert_to_database", false);

        // 1. 先查本地 DB
        json dbResult = readWorkOrderFromDB(wo);
        if (dbResult != nullptr) return crow::response(json{{"success", true}, {"source", "DB"}, {"data", dbResult}}.dump());

        // 2. 檢查連線狀態 (Fast Fail)
        // [Req 4] 若已知斷線，直接回傳錯誤，不讓前端空等
        if (!g_isMesOnline) {
            return crow::response(json{{"success", false}, {"type", "mes_offline"}, {"message", "因與 IT server 網路中斷，因此工單查詢失敗"}}.dump());
        }

        // 3. 嘗試 CMD 235
        string res235 = SoapClient::sendRequest(235, emp, wo);
        
        // [Req 4] 二次檢查：如果回傳空字串，代表連線剛剛超時或失敗了
        if (res235.empty() && !g_isMesOnline) {
            return crow::response(json{{"success", false}, {"type", "mes_offline"}, {"message", "因與 IT server 網路中斷，因此工單查詢失敗"}}.dump());
        }

        WorkOrderData d235 = parseSoapResponse(res235, wo, 235);
        if (d235.valid) {
            if (insertDB) saveWorkOrderToDB(d235);
            json j; j["workorder"] = d235.workorder; j["item"] = d235.item; j["workStep"] = d235.workStep; j["panel_num"] = d235.panel_num;
            j["sht_no"] = d235.sht_no; j["panel_no"] = d235.panel_no; j["twodid_step"] = d235.twodid_step; j["twodid_type"] = d235.twodid_type;
            j["scanned_data"] = nullptr; 
            return crow::response(json{{"success", true}, {"source", "API235"}, {"data", j}}.dump());
        }

        // 4. 嘗試 CMD 236
        // 如果 235 只是查無資料(但連線正常)，才繼續查 236
        string res236 = SoapClient::sendRequest(236, emp, wo);

        // [Req 4] 同樣檢查 236 的連線狀況
        if (res236.empty() && !g_isMesOnline) {
            return crow::response(json{{"success", false}, {"type", "mes_offline"}, {"message", "因與 IT server 網路中斷，因此工單查詢失敗"}}.dump());
        }

        WorkOrderData d236 = parseSoapResponse(res236, wo, 236);
        if (d236.valid) {
            if (insertDB) saveWorkOrderToDB(d236);
            json j; j["workorder"] = d236.workorder; j["item"] = d236.item; j["workStep"] = d236.workStep; j["panel_num"] = d236.panel_num;
            j["sht_no"] = d236.sht_no; j["panel_no"] = d236.panel_no; j["twodid_step"] = d236.twodid_step; j["twodid_type"] = d236.twodid_type;
            j["scanned_data"] = nullptr;
            return crow::response(json{{"success", true}, {"source", "API236"}, {"data", j}}.dump());
        }

        return crow::response(json{{"success", false}, {"message", "查無資料"}}.dump());
    });

    // API 3: CMD 238
    CROW_ROUTE(app, "/api/twodid").methods(crow::HTTPMethod::Post)
    ([](const crow::request& req){
        auto x = json::parse(req.body);
        
        // [Req 4] 檢查連線狀態
        if (!g_isMesOnline) return crow::response(json{{"success", false}, {"type", "mes_offline"}, {"message", "因與 IT server 網路中斷，因此 2DID 資訊查詢失敗"}}.dump());

        string raw = SoapClient::sendRequest(238, x["emp_no"], x["twodid"]);

        // [Req 4] 檢查是否因為 timeout 導致回傳空字串
        if (raw.empty() && !g_isMesOnline) return crow::response(json{{"success", false}, {"type", "mes_offline"}, {"message", "因與 IT server 網路中斷，因此 2DID 資訊查詢失敗"}}.dump());
        if (raw.find("OK") == 0) return crow::response(json{{"success", true}, {"result", {{"result", raw}}}}.dump());
        return crow::response(json{{"success", false}, {"message", "Not Found"}}.dump());
    });

    // API 4: CMD 239 Single
    // ✅ [Req 3] 使用 SafeSoapCall 取代直接的 sendRequest
    CROW_ROUTE(app, "/api/write2did").methods(crow::HTTPMethod::Post)
    ([](const crow::request& req){
        auto x = json::parse(req.body);
        string emp = x["emp_no"];
        string item = x.value("item", "NA");
        string step = x.value("workStep", "NA");
        string type = x.value("twodid_type", "Y");
        string status = x.value("remark", "異常狀態");
        string timestamp = x.value("timestamp", getCurrentDateTimeStr());

        type = (type == "OK") ? "N" : "Y";
        status = (type == "N") ? "OK" : x["remark"];
        
        string msg = string(x["workOrder"]) + ";" + item + ";" + step + ";" + string(x["sht_no"]) + ";" + string(x["panel_no"]) + ";" + step + ";" + timestamp + ";" + type + ";" + status + ";;";

        // 如果失敗或離線，會自動轉存 DB
        SafeSoapCall(emp, msg);
        
        vector<ScannedData> list;
        list.push_back({x["workOrder"], x["sht_no"], x["panel_no"], x["twodid_type"], x["remark"], 
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()});
        saveScannedListToDB(list);

        return crow::response(json{{"success", true}}.dump());
    });

    // API 5: Batch
    // ✅ [Req 3] 大幅修改：支援斷線時直接存 DB (Fast Path)
    CROW_ROUTE(app, "/api/write2dids").methods(crow::HTTPMethod::Post)
    ([](const crow::request& req){
        auto listJson = json::parse(req.body); 
        if (!listJson.is_array()) return crow::response(400);

        string emp = listJson[0]["emp_no"]; 
        vector<ScannedData> dbList;
        vector<future<void>> futures; 

        for (const auto& x : listJson) {
            string wo = x.value("workOrder", "");
            string sht = x.value("sht_no", "");
            string pnl = x.value("panel_no", "");
            string ret = x.value("twodid_type", "Y");
            string rem = x.value("remark", "異常錯誤");
            string item = x.value("item", "NA");
            string step = x.value("workStep", "NA");
            string timestamp = x.value("timestamp", getCurrentDateTimeStr());

            ret = (ret == "OK") ? "N" : "Y";
            if (ret == "N") rem = "OK";
            
            string msg = wo + ";" + item + ";" + step + ";" + sht + ";" + pnl + ";" + step + ";" + timestamp + ";" + ret + ";" + rem + ";;";
            
            // 策略判斷：
            // 1. 如果 g_isMesOnline = true，開 Thread 去送 (失敗還是會進 DB)
            // 2. 如果 g_isMesOnline = false，直接在主執行緒存 DB (效率最高，不開 Thread)
            if (g_isMesOnline) {
                futures.push_back(std::async(std::launch::async, [emp, msg](){ 
                    SafeSoapCall(emp, msg); 
                }));
            } else {
                saveUnsentMessage(emp, msg);
            }

            dbList.push_back({wo, sht, pnl, ret, rem, std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()});

            if (futures.size() >= 10) {
                for (auto& f : futures) f.get();
                futures.clear();
            }
        }

        for (auto& f : futures) f.get(); 
        saveScannedListToDB(dbList);

        return crow::response(json{{"success", true}, {"count", dbList.size()}}.dump());
    });

    // API 6: Delete (保持不變)
    CROW_ROUTE(app, "/api/Delete_2DID").methods(crow::HTTPMethod::Post)
    ([](const crow::request& req){
        auto x = json::parse(req.body);
        string wo = x["workorder"];
        MYSQL* con = dbPool->getConnection();
        if (con) {
            string sql = "DELETE FROM 2DID_workorder WHERE work_order = '" + sql_escape(wo) + "'";
            mysql_query(con, sql.c_str());
            dbPool->releaseConnection(con);
        }
        return crow::response(json{{"success", true}}.dump());
    });

    app.port(2151).multithreaded().run();
}