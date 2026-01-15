// BackendService.cpp
#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN 
    #define NOMINMAX
#endif

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
#include <algorithm>
#include <set>

using json = nlohmann::json;
using namespace std;

// --- 配置區 ---
const char* DB_HOST = "10.8.32.64";
const int   DB_PORT = 3306;
const char* DB_USER = "sfuser";
const char* DB_PASS = "1q2w3e4R"; 
const char* DB_NAME = "sfdb4070"; 

const string IIS_API_URL = "http://ksrv-web-ap3.flexium.local/gxfirstOIS/gxfirstOIS.asmx/GetOISData";
const string SOAP_URL = "http://10.8.1.124/MESConnect.svc";
const string SOAP_ACTION = "http://tempuri.org/IMESConnect/UpLoadImage";

// ✅ [Req 2] 全域變數：MES 連線狀態
std::atomic<bool> g_isMesOnline{true};

// --- 資料結構 ---
struct WorkOrderData {
    string workorder, item, workStep;
    int panel_num = 0;
    vector<string> sht_no, panel_no, twodid_step, twodid_type;
    bool cmd236_flag = false;
    bool valid = false;
};

struct ScannedData {
    string workOrder, sht_no, panel_no, ret_type, status;
    long long timestamp;
};

// --- Simple Thread Pool ---
class ThreadPool {
    vector<thread> workers;
    queue<function<void()>> tasks;
    mutex queue_mutex;
    condition_variable condition;
    bool stop;
public:
    ThreadPool(size_t threads) : stop(false) {
        for(size_t i = 0; i<threads; ++i)
            workers.emplace_back([this] {
                for(;;) {
                    function<void()> task;
                    {
                        unique_lock<mutex> lock(this->queue_mutex);
                        this->condition.wait(lock, [this]{ return this->stop || !this->tasks.empty(); });
                        if(this->stop && this->tasks.empty()) return;
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }
                    task();
                }
            });
    }
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> future<typename result_of<F(Args...)>::type> {
        using return_type = typename result_of<F(Args...)>::type;
        auto task = make_shared<packaged_task<return_type()>>(bind(forward<F>(f), forward<Args>(args)...));
        future<return_type> res = task->get_future();
        {
            unique_lock<mutex> lock(queue_mutex);
            if(stop) throw runtime_error("enqueue on stopped ThreadPool");
            tasks.emplace([task](){ (*task)(); });
        }
        condition.notify_one();
        return res;
    }
    ~ThreadPool() {
        { unique_lock<mutex> lock(queue_mutex); stop = true; }
        condition.notify_all();
        for(thread &worker: workers) worker.join();
    }
};

// 全域執行緒池 (建議 4-8 個執行緒)
ThreadPool g_threadPool(4);

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

        // 關閉 SSL 驗證 (解決 0x800B0109) for old version MySQL Connector
        // my_bool ssl_verify = 0; 
        // mysql_options(con, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &ssl_verify);

        // 關閉 SSL 驗證 (解決 0x800B0109) for new version MySQL Connector
        bool ssl_verify = false;
        unsigned int ssl_mode = SSL_MODE_DISABLED;
        mysql_options(con, MYSQL_OPT_SSL_MODE, &ssl_mode);
        if (mysql_real_connect(con, host.c_str(), user.c_str(), pass.c_str(), db.c_str(), port, NULL, 0) == NULL) {
            mysql_close(con);
            return nullptr;
        }
        return con;
    }
    MYSQL* getConnection() {
        PooledConn pConn;
        bool needCreate = false;
        
        {
            // 鎖的範圍僅限於操作 queue
            lock_guard<mutex> lock(m_mutex);
            if (pool.empty()) {
                needCreate = true;
            } else {
                pConn = pool.front();
                pool.pop();
            }
        }

        // 在鎖外進行連線建立 (耗時操作)
        if (needCreate) {
            return createConnection();
        }

        // 在鎖外進行 Ping (耗時操作)
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
    out.reserve(str.size() * 2); // 預留 2 倍空間，避免 append 時重新分配
    for (char c : str) {
        if (c == '\'') out += "\\'";
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    return out;
}

// 取代原有的 getCurrentDateTimeStr
string getCurrentDateTimeStr() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    #ifdef _WIN32
        localtime_s(&tm_buf, &t);
    #else
        localtime_r(&t, &tm_buf);
    #endif
    char buf[20];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return string(buf);
}

// --- SOAP Client (優化版) ---
class SoapClient {
public:
    static string buildXml(int command, const string& emp_no, const string& message) {
        return R"(<soapenv:Envelope xmlns:soapenv="http://schemas.xmlsoap.org/soap/envelope/" xmlns:tem="http://tempuri.org/"><soapenv:Header/><soapenv:Body><tem:UpLoadImage><tem:command>)" 
               + to_string(command) + R"(</tem:command><tem:emp_no>)" + emp_no + R"(</tem:emp_no><tem:message>)" 
               + message + R"(</tem:message><tem:image_byte>null</tem:image_byte><tem:result></tem:result></tem:UpLoadImage></soapenv:Body></soapenv:Envelope>)";
    }

    // ✅ [效能優化] 使用 thread_local 讓每個執行緒重用自己的連線 Session
    static string sendRequest(int command, const string& emp_no, const string& message) {
        // 1. 定義 thread_local 的 Session，只有第一次執行會初始化，之後會重複使用
        static thread_local std::shared_ptr<cpr::Session> session;
        
        if (!session) {
            session = std::make_shared<cpr::Session>();
            session->SetUrl(cpr::Url{SOAP_URL});
            // 設定 Keep-Alive 很重要
            session->SetHeader(cpr::Header{
                {"Content-Type", "text/xml;charset=utf-8"},
                {"SOAPAction", SOAP_ACTION},
                {"Connection", "keep-alive"} 
            });
            session->SetTimeout(cpr::Timeout{3000}); // 3秒超時
        }

        // 2. 每次只更新 Body，不需要重新設定 URL 和 Header
        session->SetBody(cpr::Body{buildXml(command, emp_no, message)});

        // 3. 發送請求
        cpr::Response r = session->Post();

        if (r.error.code != cpr::ErrorCode::OK || r.status_code != 200) {
            // 如果連線失敗，我們可以考慮重置 session (視情況而定，這裡簡單處理)
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

    // Ping Server 也可以共用 Session (或者為了輕量化維持獨立 GET 也可以)
    static bool pingServer() {
        cpr::Response r = cpr::Get(cpr::Url{SOAP_URL}, cpr::Timeout{500});
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
    data.cmd236_flag = (cmdType == 236);
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
            data.panel_num = std::stoi(parts[8 + offset].substr(1));
        }
    }
    if (cmdType == 235) data.panel_num = std::set<std::string>(data.panel_no.begin(), data.panel_no.end()).size();
    data.valid = true;
    return data;
}

// --- DB Helper Functions (保持不變) ---
// ✅ [安全修正] 改用 Prepared Statement (saveWorkOrderToDB)
void saveWorkOrderToDB(const WorkOrderData& d) {
    MYSQL* con = dbPool->getConnection();
    if (!con) return;

    // 1. 寫入 WorkOrder 主表
    // 使用 ON DUPLICATE KEY UPDATE 語法
    const char* query = "INSERT INTO 2DID_workorder (work_order, product_item, work_step, panel_sum, cmd236_flag) "
                        "VALUES (?, ?, ?, ?, ?) "
                        "ON DUPLICATE KEY UPDATE product_item=?, work_step=?, panel_sum=?";
    
    MYSQL_STMT* stmt = mysql_stmt_init(con);
    if (stmt) {
        if (mysql_stmt_prepare(stmt, query, strlen(query)) == 0) {
            MYSQL_BIND bind[8];
            memset(bind, 0, sizeof(bind));
            
            // 參數準備
            unsigned long wo_len = d.workorder.length();
            unsigned long item_len = d.item.length();
            unsigned long step_len = d.workStep.length();
            int cmd_flag = d.cmd236_flag ? 1 : 0;

            // VALUES (?, ?, ?, ?, ?)
            bind[0].buffer_type = MYSQL_TYPE_STRING; bind[0].buffer = (char*)d.workorder.c_str(); bind[0].length = &wo_len;
            bind[1].buffer_type = MYSQL_TYPE_STRING; bind[1].buffer = (char*)d.item.c_str();      bind[1].length = &item_len;
            bind[2].buffer_type = MYSQL_TYPE_STRING; bind[2].buffer = (char*)d.workStep.c_str();  bind[2].length = &step_len;
            bind[3].buffer_type = MYSQL_TYPE_LONG;   bind[3].buffer = (char*)&d.panel_num;
            bind[4].buffer_type = MYSQL_TYPE_LONG;   bind[4].buffer = (char*)&cmd_flag;
            
            // UPDATE product_item=?, work_step=?, panel_sum=?
            bind[5].buffer_type = MYSQL_TYPE_STRING; bind[5].buffer = (char*)d.item.c_str();      bind[5].length = &item_len;
            bind[6].buffer_type = MYSQL_TYPE_STRING; bind[6].buffer = (char*)d.workStep.c_str();  bind[6].length = &step_len;
            bind[7].buffer_type = MYSQL_TYPE_LONG;   bind[7].buffer = (char*)&d.panel_num;

            mysql_stmt_bind_param(stmt, bind);
            mysql_stmt_execute(stmt);
        }
        mysql_stmt_close(stmt);
    }

    // 2. 刪除舊的預期產品資料
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

    // 3. 批量寫入新的預期產品 (如果有)
    if (!d.sht_no.empty()) {
        mysql_query(con, "START TRANSACTION"); // 批量寫入開啟事務加速
        
        // 這裡為了保持代碼簡潔且高效，我們可以重複利用 stmt
        // 或者因為是 Batch Insert，用 Prepared Statement 迴圈執行也是很快的
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
                    strncpy(sht_buf, d.sht_no[i].c_str(), sizeof(sht_buf)); sht_len = d.sht_no[i].length();
                    strncpy(pnl_buf, d.panel_no[i].c_str(), sizeof(pnl_buf)); pnl_len = d.panel_no[i].length();
                    strncpy(step_buf, d.twodid_step[i].c_str(), sizeof(step_buf)); step_len = d.twodid_step[i].length();
                    strncpy(type_buf, d.twodid_type[i].c_str(), sizeof(type_buf)); type_len = d.twodid_type[i].length();
                    
                    mysql_stmt_execute(stmt);
                }
            }
            mysql_stmt_close(stmt);
        }
        mysql_query(con, "COMMIT");
    }

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
                result["cmd236_flag"] = (stoi(row[4]) == 1) ? true : false;
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

// ✅ [安全修正] 改用 Prepared Statement，防止 SQL Injection
void saveUnsentMessage(const string& emp, const string& msg) {
    MYSQL* con = dbPool->getConnection();
    if (!con) return;
    
    // 使用 ? 佔位符
    const char* query = "INSERT INTO 2DID_unsent_messages (emp_no, message) VALUES (?, ?)";
    
    MYSQL_STMT* stmt = mysql_stmt_init(con);
    if (!stmt) { dbPool->releaseConnection(con); return; }
    
    if (mysql_stmt_prepare(stmt, query, strlen(query))) {
        // 記錄錯誤 log
        cerr << "[DB Error] Prepare failed: " << mysql_stmt_error(stmt) << endl;
        mysql_stmt_close(stmt);
        dbPool->releaseConnection(con);
        return;
    }

    MYSQL_BIND bind[2];
    memset(bind, 0, sizeof(bind));
    unsigned long emp_len = emp.length();
    unsigned long msg_len = msg.length();

    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = (char*)emp.c_str();
    bind[0].length = &emp_len;

    bind[1].buffer_type = MYSQL_TYPE_STRING;
    bind[1].buffer = (char*)msg.c_str();
    bind[1].length = &msg_len;

    if (mysql_stmt_bind_param(stmt, bind)) {
        cerr << "[DB Error] Bind failed: " << mysql_stmt_error(stmt) << endl;
    } else {
        if (mysql_stmt_execute(stmt)) {
            cerr << "[DB Error] Execute failed: " << mysql_stmt_error(stmt) << endl;
        }
    }

    mysql_stmt_close(stmt);
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
        try {
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
                                break; // 跳出 while 迴圈
                            } else {
                                // 只有成功才加入待刪除列表
                                processed_ids.push_back(id);
                            }
                        }
                        mysql_free_result(res);

                        // ✅ [修正] 將刪除邏輯放在這裡，確保即使 break 也能刪除已成功的 ID
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
                        
                        // 如果 break 了 (g_isMesOnline == false)，這裡不做 sleep，讓外層迴圈立刻進入離線 Ping 模式
                        if (processed_ids.empty() && g_isMesOnline) {
                            // 沒有資料需要處理，且目前連線正常，才休息久一點
                            std::this_thread::sleep_for(std::chrono::seconds(5));
                        }
                    }
                }
                if (con) dbPool->releaseConnection(con);
                
                // 避免 DB 忙碌迴圈，稍微休息
                if (g_isMesOnline) std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        } catch (const std::exception& e) {
            cerr << "[Monitor Thread Error] Exception: " << e.what() << endl;
            std::this_thread::sleep_for(std::chrono::seconds(5)); // 出錯後休息一下再重試
        } catch (...) {
            cerr << "[Monitor Thread Error] Unknown Exception" << endl;
            std::this_thread::sleep_for(std::chrono::seconds(5));
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
    CROW_ROUTE(app, "/heartbeat").methods(crow::HTTPMethod::Get) ([](){
        return crow::response(json{{"MES_alive", g_isMesOnline.load()}}.dump());
    });

    // ✅ [Req 5] C++ Proxy API for Employee Validation
    // 前端呼叫此 API -> C++ 轉發給 IIS -> 回傳結果給前端
    CROW_ROUTE(app, "/api/validate_emp").methods(crow::HTTPMethod::Post) ([](const crow::request& req){
        // 1. 解析前端傳來的 JSON
        string empId;
        try {
            auto x = json::parse(req.body);
            empId = x.value("empId", "");
        } catch (const std::exception& e) {
            cout << "[Proxy] JSON Parse Error: " << e.what() << endl;
            return crow::response(400, "Invalid JSON format");
        }

        if (empId.empty()) {
            cout << "[Proxy] Missing empId" << endl;
            return crow::response(400, "Missing empId");
        }

        cout << "[Proxy] Forwarding request for EmpID: " << empId << endl;
        
        // 2. 建構 IIS ASMX 需要的參數 (模擬 Form Data) 建構內層的 JSON 字串: {"Emp_NO": "12345"}
        json innerJson;
        innerJson["Emp_NO"] = empId;
        string innerJsonStr = innerJson.dump();

        // 3. 使用 CPR 發送請求給 IIS
        cpr::Response r = cpr::Post(
            cpr::Url{IIS_API_URL},
            cpr::Payload{ {"CmdCode", "5"}, {"InMessage_Json", innerJsonStr} },
            cpr::Timeout{3000} // 設定 3 秒超時
        );

        // 4. 處理回應
        if (r.status_code == 200) {
            crow::response res(r.text);
            res.add_header("Content-Type", "application/json"); 
            return res;
        } else {
            cout << "[Proxy] IIS Failed. Status: " << r.status_code << " | Error: " << r.error.message << " | Body: " << r.text << endl;
            
            // 回傳 502 Bad Gateway 給前端，並附上錯誤訊息
            return crow::response(502, json{ {"success", false}, {"message", "IIS Server Error: " + to_string(r.status_code)} }.dump());
        }
    });

    // API 1: Write DB (保持不變)
    CROW_ROUTE(app, "/write_to_database").methods(crow::HTTPMethod::Post) ([](const crow::request& req){
        auto x = json::parse(req.body);
        WorkOrderData d;
        d.workorder = x.value("workorder", "");
        d.item = x.value("item", "");
        d.workStep = x.value("workStep", "");
        d.panel_num = x.value("panel_num", 0);
        d.cmd236_flag = x.value("cmd236_flag", 0);
        if (x.contains("sht_no")) d.sht_no = x["sht_no"].get<vector<string>>();
        if (x.contains("panel_no")) d.panel_no = x["panel_no"].get<vector<string>>();
        if (x.contains("twodid_step")) d.twodid_step = x["twodid_step"].get<vector<string>>();
        if (x.contains("twodid_type")) d.twodid_type = x["twodid_type"].get<vector<string>>();
        saveWorkOrderToDB(d);
        return crow::response(json{{"success", true}}.dump());
    });

    // API 2: Read WorkOrder
    CROW_ROUTE(app, "/api/workorder").methods(crow::HTTPMethod::Post) ([](const crow::request& req){
        auto x = json::parse(req.body);
        string wo = x.value("workorder", "");
        string emp = x.value("emp_no", "");
        bool insertDB = x.value("insert_to_database", false);

        cout << "workorder: " << wo << endl;
        cout << "employee ID: " << emp << endl;
        cout << "insertDB: " << insertDB << endl;

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
            json j; j["workorder"] = d235.workorder; j["item"] = d235.item; j["workStep"] = d235.workStep; j["panel_num"] = d235.panel_num; j["cmd236_flag"] = d235.cmd236_flag;
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
            json j; j["workorder"] = d236.workorder; j["item"] = d236.item; j["workStep"] = d236.workStep; j["panel_num"] = d236.panel_num; j["cmd236_flag"] = d236.cmd236_flag;
            j["sht_no"] = d236.sht_no; j["panel_no"] = d236.panel_no; j["twodid_step"] = d236.twodid_step; j["twodid_type"] = d236.twodid_type;
            j["scanned_data"] = nullptr;
            return crow::response(json{{"success", true}, {"source", "API236"}, {"data", j}}.dump());
        }

        return crow::response(json{{"success", false}, {"message", res236}}.dump());
    });

    // API 3: CMD 238
    CROW_ROUTE(app, "/api/twodid").methods(crow::HTTPMethod::Post) ([](const crow::request& req){
        auto x = json::parse(req.body);
        
        // [Req 4] 檢查連線狀態
        if (!g_isMesOnline) return crow::response(json{{"success", false}, {"type", "mes_offline"}, {"message", "因與 IT server 網路中斷，因此 2DID 資訊查詢失敗"}}.dump());

        string raw = SoapClient::sendRequest(238, x["emp_no"], x["twodid"]);

        // [Req 4] 檢查是否因為 timeout 導致回傳空字串
        if (raw.empty() && !g_isMesOnline) return crow::response(json{{"success", false}, {"type", "mes_offline"}, {"message", "因與 IT server 網路中斷，因此 2DID 資訊查詢失敗"}}.dump());
        if (raw.find("OK") == 0) return crow::response(json{{"success", true}, {"result", {{"result", raw}}}}.dump());
        return crow::response(json{{"success", false}, {"message", "Not Found"}}.dump());
    });

    // ✅ [新增] 驗證用的 Helper Lambda，避免重複寫邏輯
    auto isValidInput = [](const string& wo, const string& sht, const string& pnl) -> bool {
        // 1. 檢查是否為空
        if (wo.empty() || sht.empty() || pnl.empty()) return false;

        // 2. 檢查 WorkOrder 長度 (9-10) 與 英數字組合
        if (wo.length() < 9 || wo.length() > 10) return false;
        bool isAlnum = std::all_of(wo.begin(), wo.end(), [](unsigned char c){ return std::isalnum(c); });
        if (!isAlnum) return false;

        // 3. 檢查 sht_no 與 panel_no 長度 (必須為 13)
        if (sht.length() != 13) return false;
        if (pnl.length() != 13) return false;

        return true;
    };

    // ✅ [新增] 時間格式驗證 Helper
    auto isValidDateTime = [](const string& dt) -> bool {
        // 格式必須為 "YYYY-MM-DD HH:MM:SS" (長度 19)
        if (dt.length() != 19) return false;
        // 檢查分隔符號
        if (dt[4] != '-' || dt[7] != '-' || dt[10] != ' ' || dt[13] != ':' || dt[16] != ':') return false;
        // 檢查是否全為數字 (排除分隔符號)
        for (int i = 0; i < 19; ++i) {
            if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16) continue;
            if (!isdigit(dt[i])) return false;
        }
        return true;
    };

    // API 4: CMD 239 Single
    // ✅ [Req 3] 使用 SafeSoapCall 取代直接的 sendRequest
    // ✅ [修改] 新增 entryTime 與 exitTime 邏輯
    CROW_ROUTE(app, "/api/write2did").methods(crow::HTTPMethod::Post) ([&isValidInput, &isValidDateTime](const crow::request& req){
        try {
            auto x = json::parse(req.body);
            
            string emp = x.value("emp_no", "");
            string wo = x.value("workOrder", "");
            string sht = x.value("sht_no", "");
            string pnl = x.value("panel_no", "");

            // 1. 基礎欄位驗證
            if (!isValidInput(wo, sht, pnl)) {
                return crow::response(400, json{{"success", false}, {"message", "Invalid format: Check WorkOrder(9-10 alnum) or Sheet/Panel No(13)"}}.dump());
            }

            // 2. entryTime 驗證 (必填)
            string entryTime = x.value("entryTime", "");
            if (entryTime.empty() || !isValidDateTime(entryTime)) {
                return crow::response(400, json{{"success", false}, {"message", "Invalid or missing entryTime. Required format: YYYY-MM-DD HH:MM:SS"}}.dump());
            }

            // 3. exitTime 處理 (選填，預設為現在)
            string exitTime = x.value("exitTime", "");
            if (exitTime.empty()) {
                exitTime = getCurrentDateTimeStr();
            }

            string item = x.value("item", "NA");
            string step = x.value("workStep", "NA");
            string type = x.value("twodid_type", "Y");
            string status = x.value("remark", "異常狀態");
            
            // 轉換 type: OK -> N, 其他 -> Y
            string type_code = (type == "OK" || type == "N") ? "N" : "Y";
            
            // ✅ [修改] 更新 SOAP 訊息格式：加入 entryTime 與 exitTime
            // 格式: WO;ITEM;STEP;SHT;PNL;STEP;ENTRY_TIME;EXIT_TIME;TYPE;STATUS;;
            string msg = wo + ";" + item + ";" + step + ";" + sht + ";" + pnl + ";" + step + ";" + entryTime + ";" + exitTime + ";" + type_code + ";" + status + ";;";

            // 如果失敗或離線，會自動轉存 DB
            SafeSoapCall(emp, msg);
            
            vector<ScannedData> list;
            list.push_back({wo, sht, pnl, x["twodid_type"], x["remark"], 
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()});
            saveScannedListToDB(list);

            return crow::response(json{{"success", true}, {"mes_status", g_isMesOnline ? "online" : "offline"}}.dump());
        } catch (const std::exception& e) {
            return crow::response(400, json{{"success", false}, {"message", "Invalid JSON format"}}.dump());
        }
    });

    // API 5: Batch
    // ✅ [Req 3] 大幅修改：支援斷線時直接存 DB (Fast Path)
    // ✅ [修改] 新增 entryTime 與 exitTime 邏輯
    CROW_ROUTE(app, "/api/write2dids").methods(crow::HTTPMethod::Post) ([&isValidInput, &isValidDateTime](const crow::request& req){
        try{
            auto listJson = json::parse(req.body); 
            if (!listJson.is_array()) return crow::response(400);

            if (listJson.empty()) return crow::response(200, json{{"success", true}, {"count", 0}}.dump());

            string emp = listJson[0].value("emp_no", ""); 
            vector<ScannedData> dbList;
            vector<future<void>> futures; 

            for (const auto& x : listJson) {
                string wo = x.value("workOrder", "");
                string sht = x.value("sht_no", "");
                string pnl = x.value("panel_no", "");

                // 驗證 1: 基礎格式
                if (!isValidInput(wo, sht, pnl)) {
                    continue; // 略過格式錯誤的資料
                }

                // 驗證 2: entryTime (必填)
                string entryTime = x.value("entryTime", "");
                if (entryTime.empty() || !isValidDateTime(entryTime)) {
                    // Batch 模式下，若時間格式錯誤則略過該筆 (或視需求改為 return 400)
                    cout << "[Batch Error] Skipping item due to invalid entryTime: " << entryTime << endl;
                    continue; 
                }

                // 處理 3: exitTime (選填)
                string exitTime = x.value("exitTime", "");
                if (exitTime.empty()) {
                    exitTime = getCurrentDateTimeStr();
                }

                string ret = x.value("twodid_type", "Y");
                string rem = x.value("remark", "異常錯誤");
                string item = x.value("item", "NA");
                string step = x.value("workStep", "NA");

                string type_code = (ret == "OK") ? "N" : "Y";
                
                // ✅ [修改] 更新 SOAP 訊息格式
                string msg = wo + ";" + item + ";" + step + ";" + sht + ";" + pnl + ";" + step + ";" + entryTime + ";" + exitTime + ";" + type_code + ";" + rem + ";;";
                
                if (g_isMesOnline) {
                    futures.push_back(g_threadPool.enqueue([emp, msg](){
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

            return crow::response(json{{"success", true}, {"count", dbList.size()}, {"mes_status", g_isMesOnline ? "online" : "offline"}}.dump());
        } catch (const std::exception& e) { 
            cout << "[API Error] write2dids JSON Parse Error: " << e.what() << endl;
            return crow::response(400, "Invalid JSON Format");
        }
    });

    // API 6: Delete
    // ✅ [安全修正] 加上 try-catch 並防止 SQL Injection
    CROW_ROUTE(app, "/api/Delete_2DID").methods(crow::HTTPMethod::Post) ([](const crow::request& req){
        try {
            auto x = json::parse(req.body);
            string wo = x.value("workorder", "");
            
            if (wo.empty()) return crow::response(400, "Missing workorder");

            MYSQL* con = dbPool->getConnection();
            if (con) {
                // 改用 Prepared Statement 刪除
                const char* query = "DELETE FROM 2DID_workorder WHERE work_order = ?";
                MYSQL_STMT* stmt = mysql_stmt_init(con);
                if (stmt) {
                    if (mysql_stmt_prepare(stmt, query, strlen(query)) == 0) {
                        MYSQL_BIND bind[1];
                        memset(bind, 0, sizeof(bind));
                        unsigned long wo_len = wo.length();
                        bind[0].buffer_type = MYSQL_TYPE_STRING;
                        bind[0].buffer = (char*)wo.c_str();
                        bind[0].length = &wo_len;
                        
                        mysql_stmt_bind_param(stmt, bind);
                        mysql_stmt_execute(stmt);
                    }
                    mysql_stmt_close(stmt);
                }
                dbPool->releaseConnection(con);
            }
            return crow::response(json{{"success", true}}.dump());
        } catch (const std::exception& e) {
            return crow::response(400, "Invalid JSON");
        }
    });

    // ✅ [新增] API: Admin Login (驗證工號是否為管理員)
    CROW_ROUTE(app, "/api/admin_login").methods(crow::HTTPMethod::Post) ([](const crow::request& req){
        try {
            auto x = json::parse(req.body);
            string empId = x.value("empId", "");

            if (empId.empty()) return crow::response(400, "Missing empId");

            MYSQL* con = dbPool->getConnection();
            bool isAdmin = false;
            if (con) {
                // 查詢該工號是否存在於 admin 表中
                string sql = "SELECT id FROM 2did_admin_password WHERE empId = '" + sql_escape(empId) + "'";
                if (mysql_query(con, sql.c_str()) == 0) {
                    MYSQL_RES* res = mysql_store_result(con);
                    if (res) {
                        if (mysql_num_rows(res) > 0) isAdmin = true;
                        mysql_free_result(res);
                    }
                }
                dbPool->releaseConnection(con);
            }

            if (isAdmin) {
                return crow::response(json{{"success", true}, {"message", "Admin verified"}}.dump());
            } else {
                return crow::response(json{{"success", false}, {"message", "Permission denied"}}.dump());
            }
        } catch (...) {
            return crow::response(400, "Invalid JSON");
        }
    });

    app.port(2151).multithreaded().run();
}