#pragma once
#include <boost/asio.hpp>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <deque> // 確保引入 deque
#include <spdlog/spdlog.h>
#include "../protocols/IPlcProtocol.hpp"
#include "../protocols/MitsubishiProtocol.hpp"

using boost::asio::ip::tcp;

struct PlcCommand {
    int addr;
    bool val;
    std::function<void(std::string)> callback;
};

class PlcConnectionManager;

// 單一 PLC 連線 Session
class PlcSession : public std::enable_shared_from_this<PlcSession> {
    tcp::socket socket_;
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;

    std::deque<PlcCommand> command_queue_; 
    boost::asio::steady_timer idle_timer_; // 閒置計時器 [cite: 277]
    boost::asio::steady_timer op_timer_;   // 操作逾時計時器 [cite: 253]
    std::shared_ptr<IPlcProtocol> protocol_; // 協議解析器 [cite: 251]

    bool is_processing_ = false;           // 是否正在處理指令
    std::vector<uint8_t> read_buffer_;
    bool connected_ = false;
    std::string key_; // 紀錄自己的 IP:Port Key，用於移除
    std::function<void(std::string)> on_close_callback_; // 通知 Manager 的 callback

public:
    PlcSession(boost::asio::io_context& ioc, const std::string& type, std::string key, std::function<void(std::string)> on_close)
        : socket_(ioc), strand_(boost::asio::make_strand(ioc)), idle_timer_(ioc), op_timer_(ioc), read_buffer_(1024), key_(key), on_close_callback_(on_close) {
        if (type == "Mitsubishi") protocol_ = std::make_shared<MitsubishiProtocol>();
        else protocol_ = std::make_shared<MitsubishiProtocol>(); 
    }

    void connect(const std::string& ip, int port, std::function<void(bool)> callback) {
        auto self = shared_from_this();
        boost::asio::ip::tcp::endpoint ep(boost::asio::ip::make_address(ip), port);
        
        // 1. 設定連線逾時 (3秒)
        op_timer_.expires_after(std::chrono::seconds(3));
        op_timer_.async_wait([this, self](boost::system::error_code ec){
            if (ec != boost::asio::error::operation_aborted) {
                spdlog::warn("[PLC] Connect Timeout: {}", key_);
                boost::system::error_code ignored;
                socket_.close(ignored);
            }
        });

        socket_.async_connect(ep, [this, self, callback](boost::system::error_code ec) {
            op_timer_.cancel(); 

            if (!ec) {
                connected_ = true;
                refresh_idle_timer(); 
                callback(true);
            } else {
                spdlog::error("[PLC] Connect Failed: {} ({})", ec.message(), key_);
                callback(false);
            }
        });
    }

    void send_command(int addr, bool val, std::function<void(std::string)> api_callback) {
        auto self = shared_from_this();
        boost::asio::post(strand_, [this, self, addr, val, api_callback]() {
            command_queue_.push_back({addr, val, api_callback});
            if (!is_processing_) {
                process_next_command();
            }
        });
    }

    void close_session() {
        if (!connected_) return;
        connected_ = false;
        boost::system::error_code ec;
        socket_.close(ec);
        idle_timer_.cancel();
        op_timer_.cancel();
        spdlog::info("[PLC] Session Closed: {}", key_);
        if (on_close_callback_) on_close_callback_(key_);
    }

private:
    void process_next_command() {
        if (command_queue_.empty() || !connected_) {
            is_processing_ = false;
            return;
        }

        is_processing_ = true;
        auto& cmd = command_queue_.front();
        
        auto packet = protocol_->buildWriteBitPacket(cmd.addr, cmd.val);
        auto self = shared_from_this();

        // 啟動超時計時器
        op_timer_.expires_after(std::chrono::seconds(2));
        op_timer_.async_wait(boost::asio::bind_executor(strand_, [this, self](const boost::system::error_code& ec){
            if (ec != boost::asio::error::operation_aborted) {
                close_session(); // 逾時關閉連線
            }
        }));

        boost::asio::async_write(socket_, boost::asio::buffer(packet),
            boost::asio::bind_executor(strand_, [this, self](boost::system::error_code ec, std::size_t) {
                if (!ec) {
                    // 修正 B2: 使用 async_read 讀取固定長度回應 (MC Protocol 回應通常是 11 bytes)
                    boost::asio::async_read(socket_, boost::asio::buffer(read_buffer_, 11),
                        boost::asio::bind_executor(strand_, [this, self](boost::system::error_code ec, std::size_t len) {
                            op_timer_.cancel();
                            auto callback = command_queue_.front().callback;
                            command_queue_.pop_front();

                            if (!ec && protocol_->parseResponse(read_buffer_)) {
                                callback("OK");
                            } else {
                                callback("PLC Error or Read Failed");
                            }
                            process_next_command(); // 處理下一筆
                        }));
                } else {
                    close_session();
                }
            }));
    }

    void do_read_response(std::function<void(std::string)> api_callback) {
        auto self = shared_from_this();
        // 關鍵點：async_read_some 的回呼同樣綁定 strand
        socket_.async_read_some(boost::asio::buffer(read_buffer_),
            boost::asio::bind_executor(strand_, [this, self, api_callback](boost::system::error_code ec, std::size_t len) {
                op_timer_.cancel();
                
                if (!ec) {
                    std::vector<uint8_t> resp(read_buffer_.begin(), read_buffer_.begin() + len);
                    if (protocol_->parseResponse(resp)) {
                        refresh_idle_timer();
                        api_callback("OK");
                    } else {
                        api_callback("PLC Error Response");
                    }
                } else {
                    api_callback("Read Error");
                    close_session();
                }
            }));
    }

    void refresh_idle_timer() {
        idle_timer_.expires_after(std::chrono::minutes(5));
        auto self = shared_from_this();
        idle_timer_.async_wait([this, self](boost::system::error_code ec) {
            if (ec != boost::asio::error::operation_aborted) {
                spdlog::info("[PLC] Idle Timeout (5min). Closing: {}", key_);
                close_session();
            }
        });
    }
};

class PlcConnectionManager {
    boost::asio::io_context& ioc_;
    std::unordered_map<std::string, std::shared_ptr<PlcSession>> sessions_;
    std::mutex mutex_;

public:
    PlcConnectionManager(boost::asio::io_context& ioc) : ioc_(ioc) {}

    void write_status(const std::string& ip, int port, const std::string& type, 
                      int addr_trigger, int addr_result, bool is_ok, 
                      std::function<void(std::string)> callback) {
        
        std::string key = ip + ":" + std::to_string(port);
        std::shared_ptr<PlcSession> session;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (sessions_.count(key)) session = sessions_[key];
        }

        if (!session) {
            session = std::make_shared<PlcSession>(ioc_, type, key, [this](std::string k){
                std::lock_guard<std::mutex> lock(mutex_);
                sessions_.erase(k);
            });

            {
                std::lock_guard<std::mutex> lock(mutex_);
                sessions_[key] = session;
            }
            
            session->connect(ip, port, [session, addr_trigger, addr_result, is_ok, callback](bool success) {
                if (success) {
                    int val_result = is_ok ? 1 : 0;
                    session->send_command(addr_result, val_result == 1, [session, addr_trigger, callback](std::string res1){
                        if(res1 == "OK") session->send_command(addr_trigger, true, callback);
                        else callback(res1);
                    });
                } else {
                    callback("Connection Failed");
                }
            });
        } else {
            int val_result = is_ok ? 1 : 0;
            session->send_command(addr_result, val_result == 1, [session, addr_trigger, callback](std::string res1){
                if(res1 == "OK") session->send_command(addr_trigger, true, callback);
                else callback(res1);
            });
        }
    }
};