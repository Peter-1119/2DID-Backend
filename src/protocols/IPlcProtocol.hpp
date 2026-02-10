#pragma once
#include <vector>
#include <cstdint>
#include <string>

class IPlcProtocol {
public:
    virtual ~IPlcProtocol() = default;
    // 產生寫入點位的封包 (回傳 bytes)
    virtual std::vector<uint8_t> buildWriteBitPacket(int address, bool value) = 0;
    // 解析回應封包，確認是否成功
    virtual bool parseResponse(const std::vector<uint8_t>& response) = 0;
};