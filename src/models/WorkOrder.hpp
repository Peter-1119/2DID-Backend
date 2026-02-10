#pragma once
#include <string>
#include <vector>

struct WorkOrderData {
    std::string workorder;
    std::string item;
    std::string workStep;
    int panel_num = 0;
    std::vector<std::string> sht_no;
    std::vector<std::string> panel_no;
    std::vector<std::string> twodid_step;
    std::vector<std::string> twodid_type;
    bool cmd236_flag = false;
    bool valid = false;
};