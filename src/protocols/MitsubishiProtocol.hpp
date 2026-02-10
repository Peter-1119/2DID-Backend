#pragma once
#include "IPlcProtocol.hpp"

class MitsubishiProtocol : public IPlcProtocol {
public:
    std::vector<uint8_t> buildWriteBitPacket(int address, bool value) override {
        // 移植自 PlcClient.hpp 的 build_write_packet
        // 注意：這裡假設 address 是整數 (如 86 代表 M86)
        std::vector<uint8_t> packet = {
            0x50, 0x00, 0x00, 0xFF, 0xFF, 0x03, 0x00,
            0x0D, 0x00, // 長度
            0x10, 0x00, // Timer
            0x01, 0x14, 0x01, 0x00, // Cmd: Batch Write
            (uint8_t)(address & 0xFF), (uint8_t)((address >> 8) & 0xFF), 0x00,
            0x90, // Device M
            0x01, 0x00, // Count 1
            (uint8_t)(value ? 0x10 : 0x00) // High Nibble for 1st bit
        };
        return packet;
    }

    bool parseResponse(const std::vector<uint8_t>& response) override {
        // 檢查 End Code (MC Protocol response header)
        if (response.size() < 11) return false;
        int end_code = response[9] | (response[10] << 8);
        return (end_code == 0);
    }
};