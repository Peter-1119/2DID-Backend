// Windows 必須定義這個，避免 Winsock 衝突與 max/min 巨集干擾
#define WIN32_LEAN_AND_MEAN 
#define NOMINMAX 

#include "crow_all.h" // 請確保 crow_all.h 在同目錄
#include <mysql.h>    // 使用原生 C API
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

// --- MySQL 連線池 ---
class DbPool {
    struct PooledConn {
        MYSQL* con;
        std::chrono::steady_clock::time_point last_used;
    };

    string host, user, pass, db;
    int port;
    queue<PooledConn> pool; // 改用 struct 儲存
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
        if (con == NULL) {
            cerr << "mysql_init() failed" << endl;
            return nullptr;
        }
        
        int timeout = 3;
        mysql_options(con, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
        
        // 關閉 SSL 驗證 (解決 0x800B0109)
        my_bool ssl_verify = 0; 
        mysql_options(con, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &ssl_verify);

        if (mysql_real_connect(con, host.c_str(), user.c_str(), pass.c_str(), 
                               db.c_str(), port, NULL, 0) == NULL) {
            cerr << "mysql_real_connect() failed: " << mysql_error(con) << endl;
            mysql_close(con);
            return nullptr;
        }
        return con;
    }

    MYSQL* getConnection() {
        lock_guard<mutex> lock(m_mutex);
        if (pool.empty()) {
            return createConnection();
        }
        
        PooledConn pConn = pool.front();
        pool.pop();
        
        // 高效能優化：只有超過 30 秒沒使用的連線才檢查 Ping
        // 這樣可以大幅減少 API 延遲
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - pConn.last_used).count() > 30) {
            if (mysql_ping(pConn.con) != 0) {
                mysql_close(pConn.con); // 舊連線已斷，關閉它
                return createConnection(); // 建立新的
            }
        }
        
        return pConn.con;
    }

    void releaseConnection(MYSQL* con) {
        if (!con) return;
        lock_guard<mutex> lock(m_mutex);
        // 歸還時更新時間戳記
        pool.push({con, std::chrono::steady_clock::now()});
    }
};

shared_ptr<DbPool> dbPool;

// --- Helper: SQL Escape ---
// 使用 mysql_real_escape_string 需要連線物件，這裡用簡易版 (足以應付一般 alphanumeric)
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

// --- SOAP Client ---
class SoapClient {
public:
    static string buildXml(int command, const string& emp_no, const string& message) {
        return R"(<soapenv:Envelope xmlns:soapenv="http://schemas.xmlsoap.org/soap/envelope/" xmlns:tem="http://tempuri.org/"><soapenv:Header/><soapenv:Body><tem:UpLoadImage><tem:command>)" 
               + to_string(command) + R"(</tem:command><tem:emp_no>)" + emp_no + R"(</tem:emp_no><tem:message>)" 
               + message + R"(</tem:message><tem:image_byte>null</tem:image_byte><tem:result></tem:result></tem:UpLoadImage></soapenv:Body></soapenv:Envelope>)";
    }

    static string sendRequest(int command, const string& emp_no, const string& message) {
        cpr::Response r = cpr::Post(
            cpr::Url{SOAP_URL},
            cpr::Body{buildXml(command, emp_no, message)},
            cpr::Header{
                {"Content-Type", "text/xml;charset=utf-8"},
                {"SOAPAction", SOAP_ACTION},
                {"Connection", "keep-alive"} 
            },
            cpr::Timeout{3000}
        );

        if (r.status_code == 200) {
            string target = "<UpLoadImageResult>";
            string end_target = "</UpLoadImageResult>";
            size_t start_pos = r.text.find(target);
            size_t end_pos = r.text.find(end_target);
            if (start_pos != string::npos && end_pos != string::npos) {
                return r.text.substr(start_pos + target.length(), end_pos - start_pos - target.length());
            }
        }
        return "";
    }
};

// --- Parse SOAP Response (修正版：包含第一行數據) ---
WorkOrderData parseSoapResponse(string raw, string inputWO, int cmdType) {
    WorkOrderData data;
    // 檢查是否回傳成功
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
    data.panel_num = lines.size(); // 現在包含第一行，所以總數就是行數

    // 迴圈從 0 開始，不跳過第一行
    for (size_t i = 0; i < lines.size(); ++i) {
        stringstream lineSS(lines[i]);
        string segment;
        vector<string> parts;
        while(getline(lineSS, segment, ';')) parts.push_back(segment);

        // ✅ 關鍵修正：如果是第一行 (i==0)，因為前面多了 "OK;"，所以索引要 +1
        int offset = (i == 0) ? 1 : 0;

        // 1. 如果是第一行，順便抓取全域資訊 (Item, WorkStep)
        if (i == 0) {
            if (cmdType == 235 && parts.size() > (1 + offset)) {
                data.item = parts[0 + offset]; 
                data.workStep = parts[1 + offset];
            } else if (cmdType == 236 && parts.size() > (2 + offset)) {
                data.item = parts[1 + offset]; 
                data.workStep = parts[2 + offset];
            }
        }

        // 2. 抓取每行的製品資訊 (套用 offset)
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

// --- DB Helpers (C API) ---

void saveWorkOrderToDB(const WorkOrderData& d) {
    MYSQL* con = dbPool->getConnection();
    if (!con) return;

    mysql_query(con, "START TRANSACTION");

    // 1. Insert WorkOrder
    string sql = "INSERT INTO 2DID_workorder (work_order, product_item, work_step, panel_sum) VALUES ('" + 
                 sql_escape(d.workorder) + "','" + sql_escape(d.item) + "','" + sql_escape(d.workStep) + "'," + to_string(d.panel_num) + 
                 ") ON DUPLICATE KEY UPDATE product_item='" + sql_escape(d.item) + "', work_step='" + sql_escape(d.workStep) + "', panel_sum=" + to_string(d.panel_num);
    
    if (mysql_query(con, sql.c_str())) cerr << "Insert WO Error: " << mysql_error(con) << endl;

    // 2. Clear old expected
    string delSql = "DELETE FROM 2DID_expected_products WHERE work_order = '" + sql_escape(d.workorder) + "'";
    mysql_query(con, delSql.c_str());

    // 3. Batch Insert
    if (!d.sht_no.empty()) {
        string batchSql = "INSERT INTO 2DID_expected_products (work_order, sheet_no, panel_no, twodid_step, twodid_type) VALUES ";
        for (size_t i = 0; i < d.sht_no.size(); ++i) {
            if (i > 0) batchSql += ",";
            batchSql += "('" + sql_escape(d.workorder) + "','" + sql_escape(d.sht_no[i]) + "','" + sql_escape(d.panel_no[i]) + "','" + 
                        sql_escape(d.twodid_step[i]) + "','" + sql_escape(d.twodid_type[i]) + "')";
        }
        if (mysql_query(con, batchSql.c_str())) cerr << "Batch Insert Error: " << mysql_error(con) << endl;
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
                // Assuming columns: work_order(0), product_item(1), work_step(2), panel_sum(3)...
                // Note: Need to verify column index by name ideally, but for simplicity assuming schema order
                // Or better, select specific columns
                result["workorder"] = row[0] ? row[0] : "";
                result["item"] = row[1] ? row[1] : "";
                result["workStep"] = row[2] ? row[2] : "";
                result["panel_num"] = row[3] ? stoi(row[3]) : 0;
                mysql_free_result(res); // Free header result

                // Fetch details
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
            } else {
                mysql_free_result(res);
            }
        }
    }
    dbPool->releaseConnection(con);
    return result;
}

// --- 專業版 DB Helper (Prepared Statement) ---
void saveScannedListToDB(const vector<ScannedData>& list) {
    if (list.empty()) return;
    MYSQL* con = dbPool->getConnection();
    if (!con) return;

    // 1. 開啟交易 (這是大量寫入效能的關鍵)
    mysql_query(con, "START TRANSACTION");

    // 2. 準備 Insert 語句 (只解析一次)
    // ? 是佔位符
    const char* query = "INSERT INTO 2DID_scanned_products (work_order, sheet_no, panel_no, twodid_type, twodid_status, timestamp) VALUES (?, ?, ?, ?, ?, ?)";
    MYSQL_STMT* stmt = mysql_stmt_init(con);
    
    if (!stmt) {
        cerr << "mysql_stmt_init failed" << endl;
        dbPool->releaseConnection(con);
        return;
    }

    if (mysql_stmt_prepare(stmt, query, strlen(query))) {
        cerr << "mysql_stmt_prepare failed: " << mysql_stmt_error(stmt) << endl;
        mysql_stmt_close(stmt);
        dbPool->releaseConnection(con);
        return;
    }

    // 3. 綁定參數結構
    MYSQL_BIND bind[6];
    
    // 為了安全與效能，我們需要在迴圈外宣告緩衝區
    unsigned long str_lens[5];
    char workOrder_buf[100];
    char sheet_buf[100];
    char panel_buf[100];
    char type_buf[20];
    char status_buf[200];
    long long ts_val;

    // 初始化 bind 結構
    memset(bind, 0, sizeof(bind));

    // Col 1: work_order
    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = workOrder_buf;
    bind[0].length = &str_lens[0];

    // Col 2: sheet_no
    bind[1].buffer_type = MYSQL_TYPE_STRING;
    bind[1].buffer = sheet_buf;
    bind[1].length = &str_lens[1];

    // Col 3: panel_no
    bind[2].buffer_type = MYSQL_TYPE_STRING;
    bind[2].buffer = panel_buf;
    bind[2].length = &str_lens[2];

    // Col 4: twodid_type
    bind[3].buffer_type = MYSQL_TYPE_STRING;
    bind[3].buffer = type_buf;
    bind[3].length = &str_lens[3];

    // Col 5: twodid_status
    bind[4].buffer_type = MYSQL_TYPE_STRING;
    bind[4].buffer = status_buf;
    bind[4].length = &str_lens[4];

    // Col 6: timestamp (BigInt)
    bind[5].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[5].buffer = &ts_val;

    // 連結 Bind 結構到 Stmt
    if (mysql_stmt_bind_param(stmt, bind)) {
        cerr << "mysql_stmt_bind_param failed" << endl;
    }

    // 4. 迴圈執行 (只更新 buffer，不重新組字串)
    for (const auto& d : list) {
        // 複製資料到 buffer (使用 strncpy 避免溢位)
        strncpy(workOrder_buf, d.workOrder.c_str(), sizeof(workOrder_buf));
        str_lens[0] = d.workOrder.length();

        strncpy(sheet_buf, d.sht_no.c_str(), sizeof(sheet_buf));
        str_lens[1] = d.sht_no.length();

        strncpy(panel_buf, d.panel_no.c_str(), sizeof(panel_buf));
        str_lens[2] = d.panel_no.length();

        strncpy(type_buf, d.ret_type.c_str(), sizeof(type_buf));
        str_lens[3] = d.ret_type.length();

        strncpy(status_buf, d.status.c_str(), sizeof(status_buf));
        str_lens[4] = d.status.length();

        ts_val = d.timestamp;

        // 執行插入
        if (mysql_stmt_execute(stmt)) {
            cerr << "Stmt Execute Error: " << mysql_stmt_error(stmt) << endl;
        }
    }

    // 5. 更新統計 (這部分可以保留原本的邏輯，或者也改用 Stmt)
    // 為了簡單起見，這裡保留原邏輯，但建議同樣使用 escape
    string updateOK = "UPDATE 2DID_workorder SET OK_sum = OK_sum + 1 WHERE work_order IN (";
    string updateNG = "UPDATE 2DID_workorder SET NG_sum = NG_sum + 1 WHERE work_order IN (";
    bool hasOK = false, hasNG = false;

    for (const auto& d : list) {
        if (d.ret_type == "OK") {
            if (hasOK) updateOK += ",";
            updateOK += "'" + sql_escape(d.workOrder) + "'";
            hasOK = true;
        } else {
            if (hasNG) updateNG += ",";
            updateNG += "'" + sql_escape(d.workOrder) + "'";
            hasNG = true;
        }
    }

    if (hasOK) { updateOK += ")"; mysql_query(con, updateOK.c_str()); }
    if (hasNG) { updateNG += ")"; mysql_query(con, updateNG.c_str()); }

    // 6. 清理與提交
    mysql_stmt_close(stmt);
    mysql_query(con, "COMMIT");
    dbPool->releaseConnection(con);
}

// ---------------------------------------------------------
// ✅ [新增] CORS Middleware
// ---------------------------------------------------------
struct CORSHandler {
    struct context {};

    void before_handle(crow::request& req, crow::response& res, context& ctx) {
        // 什麼都不做，讓請求繼續處理
    }

    void after_handle(crow::request& req, crow::response& res, context& ctx) {
        // 在每個回應中加入 CORS Headers
        res.add_header("Access-Control-Allow-Origin", "*"); // 允許所有來源，開發時方便
        res.add_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        res.add_header("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Requested-With");
        
        // 如果是 OPTIONS 請求 (瀏覽器預檢)，直接回傳 204 No Content 並結束
        if (req.method == crow::HTTPMethod::Options) {
            res.code = 204;
            res.end();
            return; // 直接返回，Crow 就不會印出預設的 Log (視 Crow 版本而定，或是手動過濾 Log)
        }
    }
};

// 取得當前時間字串，格式：yyyy-MM-dd HH:mm:ss
string getCurrentDateTimeStr() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};

    // Windows 使用 localtime_s (安全性較高)
    #ifdef _WIN32
        localtime_s(&tm_buf, &t);
    #else
        localtime_r(&t, &tm_buf); // Linux
    #endif

    std::stringstream ss;
    ss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

// ✅ [修正] 自訂 Logger 類別
class CustomLogger : public crow::ILogHandler {
public:
    void log(const std::string& message, crow::LogLevel level) override {
        
        // (選擇性) 只顯示 INFO 以上的訊息
        // if (level < crow::LogLevel::Info) return;

        string levelStr;
        switch (level) {
            case crow::LogLevel::Debug:   levelStr = "DEBUG"; break;
            case crow::LogLevel::Info:    levelStr = "INFO "; break;
            case crow::LogLevel::Warning: levelStr = "WARN "; break;
            case crow::LogLevel::Error:   levelStr = "ERROR"; break;
            case crow::LogLevel::Critical:levelStr = "CRIT "; break;
        }

        // 使用你的 getCurrentDateTimeStr() 函式
        std::cout << "(" << getCurrentDateTimeStr() << ") [" << levelStr << "] " << message << std::endl;
    }
};

// --- Main Server ---
int main() {
    static CustomLogger logger;
    crow::logger::setHandler(&logger);

    dbPool = make_shared<DbPool>(DB_HOST, DB_PORT, DB_USER, DB_PASS, DB_NAME);
    
    // 設定 Log 層級 (選用)
    crow::logger::setLogLevel(crow::LogLevel::Info);

    crow::App<CORSHandler> app;

    // API 1: Write DB
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

        // 2.1 DB Check
        json dbResult = readWorkOrderFromDB(wo);
        if (dbResult != nullptr) return crow::response(json{{"success", true}, {"source", "DB"}, {"data", dbResult}}.dump());

        // 2.2 CMD 235
        string res235 = SoapClient::sendRequest(235, emp, wo);
        WorkOrderData d235 = parseSoapResponse(res235, wo, 235);
        if (d235.valid) {
            if (insertDB) saveWorkOrderToDB(d235);
            json j; j["workorder"] = d235.workorder; j["item"] = d235.item; j["workStep"] = d235.workStep; j["panel_num"] = d235.panel_num;
            j["sht_no"] = d235.sht_no; j["panel_no"] = d235.panel_no; j["twodid_step"] = d235.twodid_step; j["twodid_type"] = d235.twodid_type;
            return crow::response(json{{"success", true}, {"source", "API235"}, {"data", j}}.dump());
        }

        // 2.3 CMD 236
        string res236 = SoapClient::sendRequest(236, emp, wo);
        WorkOrderData d236 = parseSoapResponse(res236, wo, 236);
        if (d236.valid) {
            if (insertDB) saveWorkOrderToDB(d236);
            json j; j["workorder"] = d236.workorder; j["item"] = d236.item; j["workStep"] = d236.workStep; j["panel_num"] = d236.panel_num;
            j["sht_no"] = d236.sht_no; j["panel_no"] = d236.panel_no; j["twodid_step"] = d236.twodid_step; j["twodid_type"] = d236.twodid_type;
            return crow::response(json{{"success", true}, {"source", "API236"}, {"data", j}}.dump());
        }

        return crow::response(json{{"success", false}, {"message", "查無資料"}}.dump());
    });

    // API 3: CMD 238
    CROW_ROUTE(app, "/api/twodid").methods(crow::HTTPMethod::Post)
    ([](const crow::request& req){
        auto x = json::parse(req.body);
        string raw = SoapClient::sendRequest(238, x["emp_no"], x["twodid"]);
        if (raw.find("OK") == 0) return crow::response(json{{"success", true}, {"result", {{"result", raw}}}}.dump());
        return crow::response(json{{"success", false}, {"message", "Not Found"}}.dump());
    });

    // API 4: CMD 239 Single
    CROW_ROUTE(app, "/api/write2did").methods(crow::HTTPMethod::Post)
    ([](const crow::request& req){
        auto x = json::parse(req.body);
        string emp = x["emp_no"];
        string item = x.value("item", "NA");
        string step = x.value("workStep", "NA");
        
        string msg = string(x["workOrder"]) + ";" + item + ";" + step + ";" + string(x["sht_no"]) + ";" + string(x["panel_no"]) + ";" + step + ";" + getCurrentDateTimeStr() + ";" + string(x["twodid_type"]) + ";" + string(x["remark"]) + ";;";

        SoapClient::sendRequest(239, emp, msg);
        
        vector<ScannedData> list;
        list.push_back({x["workOrder"], x["sht_no"], x["panel_no"], x["twodid_type"], x["remark"], 
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()});
        saveScannedListToDB(list);

        return crow::response(json{{"success", true}}.dump());
    });

    // API 5: Batch
    CROW_ROUTE(app, "/api/write2dids").methods(crow::HTTPMethod::Post)
    ([](const crow::request& req){
        auto listJson = json::parse(req.body); 
        if (!listJson.is_array()) return crow::response(400);

        string emp = listJson[0]["emp_no"]; 
        vector<ScannedData> dbList;
        vector<future<string>> futures;

        for (const auto& x : listJson) {
            string wo = x.value("workOrder", "");
            string sht = x.value("sht_no", "");
            string pnl = x.value("panel_no", "");
            string ret = x.value("twodid_type", "");
            string rem = x.value("remark", "");
            string item = x.value("item", "NA");
            string step = x.value("workStep", "NA");
            
            string msg = wo + ";" + item + ";" + step + ";" + sht + ";" + pnl + ";" + step + ";" + getCurrentDateTimeStr() + ";" + ret + ";" + rem + ";;";
            futures.push_back(std::async(std::launch::async, [emp, msg](){ return SoapClient::sendRequest(239, emp, msg); }));

            dbList.push_back({wo, sht, pnl, ret, rem, std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()});

            // 限制：如果累積超過 10 個請求，先等待它們完成
            if (futures.size() >= 10) {
                for (auto& f : futures) f.get();
                futures.clear();
            }
        }

        for (auto& f : futures) f.get(); 
        saveScannedListToDB(dbList);

        return crow::response(json{{"success", true}, {"count", dbList.size()}}.dump());
    });

    // API 6: Delete
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