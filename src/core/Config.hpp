#pragma once
#include <string>

// DB 連線設定
const char* CFG_DB_HOST = "10.8.32.64";
const int   CFG_DB_PORT = 3306;
const char* CFG_DB_USER = "sfuser";
const char* CFG_DB_PASS = "1q2w3e4R";
const char* CFG_DB_NAME = "sfdb4070";

// API 設定
const std::string IIS_API_URL = "http://ksrv-web-ap3.flexium.local/gxfirstOIS/gxfirstOIS.asmx/GetOISData";
const std::string SOAP_URL = "http://10.8.1.124/MESConnect.svc";
const std::string SOAP_ACTION = "http://tempuri.org/IMESConnect/UpLoadImage";

// PLC 預設點位配置 (若 DB 未指定則使用此預設值)
struct PlcDefaultPoints {
    static const int UP_IN = 503;
    static const int UP_OUT = 506;
    static const int DN_IN = 542;
    static const int DN_OUT = 545;
    static const int START = 630;
    static const int WRITE_RESULT = 87;  // M87
    static const int WRITE_TRIGGER = 86; // M86
};