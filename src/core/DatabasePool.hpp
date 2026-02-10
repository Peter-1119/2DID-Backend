#pragma once
#include <mysql.h>
#include <queue>
#include <mutex>
#include <string>
#include <chrono>
#include <memory>
#include <iostream>

class DbPool {
    size_t max_size = 50; 
    std::atomic<size_t> current_opened{0}; 
    std::condition_variable cv;
    struct PooledConn {
        MYSQL* con;
        std::chrono::steady_clock::time_point last_used;
    };
    std::string host, user, pass, db;
    int port;
    std::queue<PooledConn> pool;
    std::mutex m_mutex;

public:
    DbPool(std::string h, int p, std::string u, std::string pwd, std::string d) 
        : host(h), port(p), user(u), pass(pwd), db(d) {
        // 初始化連線數不應超過 max_size
        for (int i = 0; i < 5; ++i) {
            MYSQL* con = createConnection();
            if (con) {
                pool.push({con, std::chrono::steady_clock::now()});
                current_opened++;
            }
        }
    }
    
    ~DbPool() {
        std::lock_guard<std::mutex> lock(m_mutex);
        while(!pool.empty()) {
            MYSQL* con = pool.front().con;
            pool.pop();
            mysql_close(con);
        }
    }

    MYSQL* getConnection() {
        std::unique_lock<std::mutex> lock(m_mutex);
        // 等待直到有空閒連線或可以建立新連線 (最多等3秒)
        if (!cv.wait_for(lock, std::chrono::seconds(3), [this]{ return !pool.empty() || current_opened < max_size; })) {
            return nullptr; // 超時回傳空值，由 Service 處理 503
        }

        if (pool.empty()) {
            current_opened++;
            lock.unlock(); // 解鎖以建立連線，避免阻塞其他執行緒
            MYSQL* con = createConnection();
            if (!con) {
                lock.lock();
                current_opened--;
                return nullptr;
            }
            return con;
        }

        auto pConn = pool.front();
        pool.pop();
        
        // 檢查連線是否活著 (mysql_ping)
        if (mysql_ping(pConn.con) != 0) {
            mysql_close(pConn.con);
            return createConnection();
        }
        return pConn.con;
    }

    void releaseConnection(MYSQL* con) {
        if (!con) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        pool.push({con, std::chrono::steady_clock::now()});
        cv.notify_one();
    }
    
private:
    MYSQL* createConnection() {
        MYSQL* con = mysql_init(NULL);
        if (!con) return nullptr;
        
        int timeout = 3;
        mysql_options(con, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
        
        // ✅ [修正] 相容性處理 SSL 設定
        #ifdef MYSQL_OPT_SSL_MODE
            unsigned int ssl_mode = SSL_MODE_DISABLED;
            mysql_options(con, MYSQL_OPT_SSL_MODE, &ssl_mode);
        #else
            my_bool ssl_verify = 0;
            mysql_options(con, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &ssl_verify);
        #endif
        
        if (!mysql_real_connect(con, host.c_str(), user.c_str(), pass.c_str(), db.c_str(), port, NULL, 0)) {
            mysql_close(con);
            return nullptr;
        }
        return con;
    }
};