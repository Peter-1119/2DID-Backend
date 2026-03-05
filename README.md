# LPSM 2DID Backend Service

![C++](https://img.shields.io/badge/Language-C++17-blue.svg)
![Framework](https://img.shields.io/badge/Framework-Crow-green.svg)
![Database](https://img.shields.io/badge/Database-MySQL%2FMariaDB-orange.svg)

## 📖 專案簡介 (Overview)

**Backend Service** 是 LPSM 系統的核心資料處理單元。這是一個高效能的 RESTful API 伺服器，負責連接前端 HMI (Vue.js)、本地資料庫 (MySQL) 以及遠端 MES 系統 (SOAP WebService)。

此服務採用 **C++17** 編寫，利用 **非同步 I/O** 與 **連線池 (Connection Pooling)** 技術，確保在高併發環境下（如產線批量掃描上傳）仍能保持低延遲與高穩定性。

### 🌟 核心功能 (Key Features)

* **高效能資料庫連線池 (`DbPool`)**:
    * 預熱 MySQL 連線，避免頻繁建立連線的開銷。
    * 具備自動健康檢查 (Ping) 與超時重連機制。
    * **SSL 優化**: 自動處理自簽章憑證問題 (Error 0x800B0109)。
    * **安全性**: 使用 **Prepared Statements** 防止 SQL Injection。
* **MES 系統整合 (SOAP Client)**:
    * 內建 XML 封裝與解析器，支援 MES API 235 (工單查詢), 236 (舊工單), 238 (條碼檢查), 239 (過帳)。
* **高併發批次處理**:
    * 支援 `/api/write2dids` 批次上傳接口。
    * 使用 `std::future` 與 `std::async` 進行多執行緒併發請求，並設有流量控制 (每批 10 個請求) 以保護 MES 伺服器。
* **CORS 支援**: 內建 Middleware 處理跨域請求 (Cross-Origin Resource Sharing)。

---

## ⚙️ 環境需求 (Prerequisites)

編譯與執行此專案需要 **MSYS2 (UCRT64)** 環境與以下套件：

* **Compiler**: `g++` (支援 C++17)
* **Libraries**:
    * `crow` (Micro web framework)
    * `mysql-connector-c` (Native MariaDB/MySQL Client)
    * `nlohmann-json` (JSON parser)
    * `cpr` (C++ Requests, for SOAP)
    * `openssl`

### 🔧 設定檔修改 (Configuration)

若移植到不同電腦，請在編譯前修改 `BackendService.cpp` 頂部的設定區塊：

```cpp
// --- 配置區 ---
const char* DB_HOST = "YOUR_DB_IP";  // 資料庫 IP (例如: 127.0.0.1 或 區域網路 IP)
const int   DB_PORT = 3306;
const char* DB_USER = "sfuser";      // 資料庫帳號
const char* DB_PASS = "YOUR_PASSWORD"; 
const char* DB_NAME = "sfdb4070"; 

const string SOAP_URL = "http://YOUR_MES_IP/MESConnect.svc"; // MES WebService 位址
```

---

## 🚀 編譯與執行 (Build & Run)

1. 編譯指令  
    請在 MSYS2 UCRT64 終端機中執行以下指令：
```Bash
# 進入專案目錄
cd /path/to/your/project/BackendService

# 編譯指令
g++ BackendService.cpp -o backend.exe \
    -std=c++17 -O3 \
    -D_WIN32_WINNT=0x0601 \
    -I/ucrt64/include/mariadb \
    -lcpr -lcurl -lmariadb -lws2_32 -lmswsock -lcrypt32 -lwldap32 -lssl -lcrypto
```

2. 部署依賴 (DLLs)  
    由於使用了動態連結，執行 backend.exe 需要依賴多個 DLL。請使用專用的 PowerShell 腳本打包：

```PowerShell
.\Bundle-MsysApp.ps1 -Target ".\backend.exe"
```
這會在 dist/backend 目錄下生成可攜帶的執行檔與所有必要的 DLL。

3. 執行
```Bash
./backend.exe
```

* 服務預設監聽 Port: **2151**
* 成功啟動後，您將在 Console 看到 Crow 的啟動訊息。

---

## 📡 API 介面文件 (API Documentation)
所有 API 均接收 JSON 格式請求，並回傳 JSON 格式結果。

1. 系統心跳檢測 (`GET /heartbeat`)  
    前端透過此 API 定期確認後端服務是否存活，並獲取目前後端與 MES Server 的連線狀態。

* **Response:**
```JSON
{
  "MES_alive": true  // true: 線上模式 (Online), false: 離線模式 (Offline)
}
```

2. 員工工號驗證 Proxy (POST /api/validate_emp)  
    CORS 解決方案：此 API 作為代理 (Proxy)，接收前端請求後，由 C++ 後端轉發至公司內網 IIS Server (ASMX) 進行驗證，再將結果回傳前端。解決瀏覽器直接呼叫外部 IIS 產生的跨域問題。

* **Request Body:**
```JSON
{
  "empId": "12868"
}
```

* **Response (成功 - 由 IIS 回傳):**
```JSON
{
  "code": 200,
  "data": {
    "empNo": "12868",
    "empName": "王小明"
  }
}
```

* **Response (失敗 - IIS 連線錯誤):**
```JSON
{
  "success": false,
  "message": "IIS Server Error: 500"
}
```

3. 寫入工單資料至資料庫 (`POST /write_to_database`)  
    直接將工單資訊寫入本地 MySQL 資料庫，不經過 MES 驗證。通常用於手動補單或測試。

* **Request Body:**
```JSON
{
  "workorder": "Y04900132",
  "item": "Product-A",
  "workStep": "STEP-01",
  "panel_num": 2,
  "sht_no": ["SHT001", "SHT002"],
  "panel_no": ["PNL001", "PNL002"],
  "twodid_step": ["STEP-01", "STEP-01"],
  "twodid_type": ["OK", "OK"]
}
```

* **Repsonse:**
```JSON
{
  "success": true
}
```

4. 工單驗證與查詢 (`POST /api/workorder`) 驗證工單資訊。   
**邏輯**: 優先查詢本地 DB -> 若無則查詢 MES API 235 (新工單) -> 若失敗則查詢 MES API 236 (舊工單)。  
**新增邏輯**: 當 Backend 無法連線至 MES IT Server (Timeout 或斷線) 時，將回傳特定錯誤類型 mes_offline。

* **Request Body:**
```JSON
{
  "emp_no": "XXXXX",
  "workorder": "Y04900132",
  "insert_to_database": true
}
```

* **Response (成功 - 來自 DB 或 MES):**
```JSON
{
  "success": true,
  "source": "DB",  // 來源: "DB" (含掃描紀錄) 或 "API235"/"API236" (無掃描紀錄)
  "data": {
    "workorder": "Y04900132",
    "item": "Product-A",
    "workStep": "STEP-01",
    "panel_num": 50,
    "cmd236_flag": false,
    "sht_no": ["SHT001", "SHT002", "..."],
    "panel_no": ["PNL001", "PNL002", "..."],
    "twodid_step": ["STEP-01", "STEP-01", "..."],
    "twodid_type": ["N", "Y", "..."],
    
    // [新增] 已掃描紀錄清單 (若 source 為 API235/236 則此欄位為 null)
    "scanned_data": [
      {
        "sheet_no": "SHT001",
        "panel_no": "PNL001",
        "twodid_type": "OK",
        "twodid_status": "PASS",
        "timestamp": 1708492800000
      },
      {
        "sheet_no": "SHT002",
        "panel_no": "PNL002",
        "twodid_type": "NG",
        "twodid_status": "Duplicate",
        "timestamp": 1708492850000
      }
    ]
  }
}
```

* **Response (失敗 - 查無資料):**
```JSON
{
  "success": false,
  "message": "查無資料"
}
```

* **Response (失敗 - MES 連線中斷) [NEW]:**
```JSON
{
  "success": false,
  "type": "mes_offline",
  "message": "因與 IT server 網路中斷，因此工單查詢失敗"
}
```

5. 條碼狀態查詢 (`POST /api/twodid`)  
    查詢單一 2DID 條碼在 MES 中的狀態 (呼叫 MES API 238)。

* **Request Body:**
```JSON
{
  "emp_no": "12868",
  "twodid": "TR1234567890"
}
```

* **Response (成功):**
```JSON
{
  "success": true,
  "result": { 
    "result": "OK" 
  }
}
```

* **Response (失敗 - MES 連線中斷) [NEW]:**
```JSON
{
  "success": false,
  "type": "mes_offline",
  "message": "因與 IT server 網路中斷，因此 2DID 資訊查詢失敗"
}
```

6. 單筆資料上傳 (`POST /api/write2did`)  
    上傳單一掃描結果至 MES (呼叫 MES API 239) 並寫入本地 DB 紀錄。
* **Request Body:**
```JSON
{
  "emp_no": "12868",
  "workOrder": "Y04900132",
  "sht_no": "4567123456789",
  "panel_no": "4567123456789",
  "twodid_type": "OK", 
  "remark": "OK-IPC",
  "item": "YD18379-04-A-A",
  "workStep": "96",
  "entryTime": "2016-01-15 15:18:00",
  "exitTime": "2016-01-15 15:20:00"
}
```

* **Response (成功):**
```JSON
{
  "success": true
}
```

* **Response (失敗 - MES 連線中斷) [new]:**
```JSON
{
  "success": true,
  "mes_status": "offline"
}
```

7. 批次資料上傳 (`POST /api/write2dids`)  
    **高效能接口**：同時上傳多筆資料。後端會自動以多執行緒 (Async) 並發處理，並進行流量控制 (每批次 10 筆請求)，最後一次性寫入資料庫。

* **Request Body:** (JSON Array)
```JSON
[
  {
    "emp_no": "12868",
    "workOrder": "Y04900132",
    "sht_no": "4567123456789",
    "panel_no": "4567123456789",
    "twodid_type": "OK", 
    "remark": "OK-IPC",
    "item": "YD18379-04-A-A",
    "workStep": "96",
    "entryTime": "2016-01-15 15:18:00",
    "exitTime": "2016-01-15 15:20:00"
  },
  {
    "emp_no": "12868",
    "workOrder": "Y04900132",
    "sht_no": "4567123456791",
    "panel_no": "4567123456791",
    "twodid_type": "OK", 
    "remark": "OK-IPC",
    "item": "YD18379-04-A-A",
    "workStep": "96",
    "entryTime": "2016-01-15 15:20:00",
    "exitTime": "2016-01-15 15:22:00"
  }
]
```

* **Response (成功):**
```JSON
{
  "success": true,
  "count": 2
}
```

* **Response (失敗 - MES 連線中斷) [new]:**
```JSON
{
  "success": true,
  "count": 2,
  "mes_status": "offline"
}
```

8. 清除工單資料 (`POST /api/Delete_2DID`)  
    作業結束時呼叫，清除本地資料庫中該工單的「預期清單 (Expected Products)」，但會保留「已掃描紀錄 (Scanned Products)」。

* **Request Body:**
```JSON
{
  "workorder": "Y04900132"
}
```

* **Response:**
```JSON
{
  "success": true
}
```

9. pcs 2DID 資料上傳 (`POST /api/pcs_write`)
    2DID 上傳

* **Request Body:**
```JSON
{
  "product": "P1",
  "work_order": "WO123",
  "pcs_id": "PCS0001",
  "twodid_type": "A",
  "twodid_status": "OK"
}
```

```JSON
{
  "product": "P1",
  "work_order": "WO123",
  "pcs_id": "PCS0001",
  "twodid_type": "A",
  "twodid_status": "",
  "timestamp": "2026-03-05 10:00:00"
}
```

10. 2DID 資料 Log 查看
* **Request Body:**
```JSON
{
  "product": "P1",
  "work_order": "WO123",
  "time_from": "2026-03-01 00:00:00",
  "time_to": "2026-03-31 23:59:59",
  "page": 1,
  "pageSize": 50
}
```
---

## 💾 資料庫結構 (Database Schema)
本服務依賴以下 MySQL 資料表 (InnoDB)：

`2DID_workorder`: 儲存工單基本資訊及統計。

Columns: `work_order` (PK), `product_item`, `work_step`, `panel_sum`, `OK_sum`, `NG_sum`.

`2DID_expected_products`: 儲存工單內預期要掃描的所有條碼 (從 MES 下載)。

Columns: `work_order`, `sheet_no`, `panel_no`, `twodid_step`, `twodid_type`.

`2DID_scanned_products`: 儲存實際掃描與上傳的紀錄。

Columns: `work_order`, `sheet_no`, `panel_no`, `twodid_type`, `twodid_status`, `timestamp`.

---

## ⚠️ 注意事項
1. **網路環境:** 請確保執行電腦能通過 TCP Port `3306` 連線至資料庫伺服器，並能通過 HTTP 連線至 MES 伺服器。

2. **執行緒限制:** 批次上傳 API 目前限制同時併發數為 **10**，以避免觸發 MES 防火牆規則或耗盡連線資源。

3. 錯誤處理:
* 資料庫連線使用 **自動重連機制 (Auto-Reconnect)**。
* 資料庫寫入使用 **交易 (Transaction)** 與 **Prepared Statements** 以確保資料一致性與安全性。