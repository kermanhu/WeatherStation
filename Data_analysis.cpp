#define _CRT_SECURE_NO_WARNINGS
#define NOMINMAX

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <cstdlib>
#include <ctime>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#endif

// ==================== 颜色 ====================
namespace C {
    const std::string RESET = "\033[0m";
    const std::string BOLD = "\033[1m";
    const std::string DIM = "\033[2m";
    const std::string RED = "\033[31m";
    const std::string GREEN = "\033[32m";
    const std::string YELLOW = "\033[33m";
    const std::string BLUE = "\033[34m";
    const std::string MAGENTA = "\033[35m";
    const std::string CYAN = "\033[36m";
    const std::string WHITE = "\033[37m";
    const std::string BRIGHT_RED = "\033[91m";
    const std::string BRIGHT_GREEN = "\033[92m";
    const std::string BRIGHT_YELLOW = "\033[93m";
    const std::string BRIGHT_BLUE = "\033[94m";
    const std::string BRIGHT_MAGENTA = "\033[95m";
    const std::string BRIGHT_CYAN = "\033[96m";
    const std::string BRIGHT_WHITE = "\033[97m";
}

void enableAnsi() {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD m = 0;
    GetConsoleMode(h, &m);
    m |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(h, m);
#endif
}

// ==================== 数据结构 ====================
struct Record {
    std::string timestamp;
    std::string date;
    double temp = 0, hum = 0, press = 0;
    double wind = 0, dir = 0, rain = 0;
    double dew = 0, appt = 0, wetb = 0;
};

// ==================== 工具函数 ====================
std::string repeat(const std::string& s, int n) {
    std::string out;
    for (int i = 0; i < n; i++) out += s;
    return out;
}

std::string fmt(double v, int prec) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(prec) << v;
    return ss.str();
}

int visibleLen(const std::string& s) {
    int n = 0;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\033') {
            while (i < s.size() && s[i] != 'm') i++;
            continue;
        }
        n++;
    }
    return n;
}

void padRight(const std::string& s, int w) {
    int v = visibleLen(s);
    std::cout << s;
    if (v < w) std::cout << std::string(w - v, ' ');
}

std::string normalizeTimestamp(const std::string& ts) {
    std::string s = ts;
    for (auto& c : s) if (c == '/') c = '-';

    size_t spacePos = s.find(' ');
    if (spacePos == std::string::npos) return s;

    std::string datePart = s.substr(0, spacePos);
    std::string timePart = s.substr(spacePos + 1);

    // ===== 规范化日期 =====
    std::stringstream ss(datePart);
    std::string year, month, day;
    std::getline(ss, year, '-');
    std::getline(ss, month, '-');
    std::getline(ss, day, '-');
    if (month.size() == 1) month = "0" + month;
    if (day.size() == 1)   day = "0" + day;

    // ===== 规范化时间：时:分:秒 分别补零 =====
    std::stringstream ts2(timePart);
    std::string hh, mm, ssec;
    std::getline(ts2, hh, ':');
    std::getline(ts2, mm, ':');
    std::getline(ts2, ssec, ':');

    if (hh.size() == 1)   hh = "0" + hh;
    if (mm.size() == 1)   mm = "0" + mm;
    if (ssec.empty())     ssec = "00";
    if (ssec.size() == 1) ssec = "0" + ssec;

    return year + "-" + month + "-" + day + " "
        + hh + ":" + mm + ":" + ssec;
}

std::string extractDate(const std::string& ts) {
    if (ts.size() < 10) return ts;
    return ts.substr(0, 10);
}

std::string nowTimestamp() {
    time_t t = time(nullptr);
    struct tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
    struct tm* tm_info = &tm_buf;
#else
    struct tm* tm_info = localtime(&t);
#endif
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_info);
    return std::string(buf);
}

std::string getCwd() {
    char buf[1024];
#ifdef _WIN32
    if (_getcwd(buf, sizeof(buf))) return std::string(buf);
#else
    if (getcwd(buf, sizeof(buf))) return std::string(buf);
#endif
    return "(未知)";
}

// ==================== 颜色规则 ====================
std::string cTemp(double t) {
    if (t >= 35) return C::BRIGHT_RED;
    if (t >= 30) return C::RED;
    if (t >= 25) return C::YELLOW;
    if (t >= 20) return C::GREEN;
    return C::BRIGHT_CYAN;
}
std::string cHum(double h) {
    if (h >= 95) return C::BRIGHT_BLUE;
    if (h >= 80) return C::BLUE;
    if (h >= 60) return C::GREEN;
    return C::YELLOW;
}
std::string cPress(double p) {
    if (p >= 102.0) return C::BRIGHT_CYAN;
    if (p >= 101.0) return C::CYAN;
    if (p >= 100.0) return C::GREEN;
    return C::YELLOW;
}
std::string cDewDiff(double d) {
    if (d < 0.5) return C::BRIGHT_BLUE;
    if (d < 1.5) return C::BLUE;
    if (d < 3.0) return C::GREEN;
    return C::YELLOW;
}
std::string cWind(double w) {
    if (w >= 5.0) return C::BRIGHT_RED;
    if (w >= 2.0) return C::YELLOW;
    if (w >= 0.1) return C::GREEN;
    return C::DIM;
}
std::string cRain(double r) {
    if (r > 0.5) return C::BRIGHT_BLUE;
    if (r > 0.0) return C::BLUE;
    return C::DIM;
}

// ==================== 输入工具 ====================
double inputDouble(const std::string& prompt, double defVal, bool allowEmpty = false) {
    while (true) {
        std::cout << "  " << C::DIM << "| " << C::RESET << C::WHITE << prompt
            << C::RESET << C::DIM << " [" << defVal << "]: " << C::RESET;
        std::string line;
        std::getline(std::cin, line);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(line.begin());
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (line.empty()) {
            if (allowEmpty) return defVal;
            std::cout << "  " << C::BRIGHT_RED << "[X] 不能为空，请重新输入" << C::RESET << "\n";
            continue;
        }
        try {
            size_t idx;
            double v = std::stod(line, &idx);
            if (idx != line.size()) throw std::runtime_error("trailing");
            return v;
        }
        catch (...) {
            std::cout << "  " << C::BRIGHT_RED << "[X] 无效数字，请重新输入" << C::RESET << "\n";
        }
    }
}

std::string inputString(const std::string& prompt, const std::string& defVal) {
    std::cout << "  " << C::DIM << "| " << C::RESET << C::WHITE << prompt
        << C::RESET << C::DIM << " [" << defVal << "]: " << C::RESET;
    std::string line;
    std::getline(std::cin, line);
    if (line.empty()) return defVal;
    return line;
}

// ==================== 数据管理 ====================
class WeatherDB {
public:
    std::vector<Record> records;
    std::string filename;
    bool lastLoadWasOldFormat = false;

    WeatherDB(const std::string& fn) : filename(fn) {}

    // 备份原文件，防止保存出错
    void backup() {
        std::ifstream src(filename, std::ios::binary);
        if (!src) return;
        std::ofstream bak(filename + ".bak", std::ios::binary);
        if (!bak) return;
        bak << src.rdbuf();
    }

    bool load() {
        std::ifstream fin(filename);
        if (!fin) return false;

        std::string line;
        bool firstLine = true;
        bool isOldFormat = false;

        while (std::getline(fin, line)) {
            if (line.empty()) continue;

            // 去掉 UTF-8 BOM
            if (line.size() >= 3 &&
                (unsigned char)line[0] == 0xEF &&
                (unsigned char)line[1] == 0xBB &&
                (unsigned char)line[2] == 0xBF) {
                line = line.substr(3);
            }

            // 切字段
            std::vector<std::string> f;
            std::stringstream ss(line);
            std::string field;
            while (std::getline(ss, field, ',')) f.push_back(field);
            if (f.empty()) continue;

            // ===== 第一行：判断是表头还是数据 =====
            if (firstLine) {
                firstLine = false;

                // 检测第一行第一个字符
                // 数字 → 数据行；字母/中文 → 表头
                char c0 = f[0].empty() ? ' ' : f[0][0];
                bool looksLikeHeader = !(c0 >= '0' && c0 <= '9');

                if (looksLikeHeader) {
                    // 是表头，跳过。根据字段数判断格式
                    isOldFormat = (f.size() >= 15);
                    lastLoadWasOldFormat = isOldFormat;
                    continue;
                }
                else {
                    // 是数据行，根据字段数判断格式
                    isOldFormat = (f.size() >= 15);
                    lastLoadWasOldFormat = isOldFormat;
                    // 不 continue，落到下面解析
                }
            }

            // ===== 解析数据行 =====
            Record r;
            try {
                if (isOldFormat) {
                    // 旧格式: 时间,名称,温度,原始湿度,气压,风速,风向,雨量,露点,体感,湿球,相对湿度,...
                    if (f.size() < 12) continue;
                    r.timestamp = normalizeTimestamp(f[0]);
                    r.date = extractDate(r.timestamp);
                    r.temp = std::stod(f[2]);
                    r.hum = (!f[11].empty()) ? std::stod(f[11]) : std::stod(f[3]);
                    r.press = std::stod(f[4]);
                    r.wind = std::stod(f[5]);
                    r.dir = std::stod(f[6]);
                    r.rain = std::stod(f[7]);
                    r.dew = std::stod(f[8]);
                    r.appt = std::stod(f[9]);
                    r.wetb = std::stod(f[10]);
                }
                else {
                    // 新格式: timestamp,temp,hum,press,wind,dir,rain,dew,appt,wetb
                    if (f.size() < 10) continue;
                    r.timestamp = normalizeTimestamp(f[0]);
                    r.date = extractDate(r.timestamp);
                    r.temp = std::stod(f[1]);
                    r.hum = std::stod(f[2]);
                    r.press = std::stod(f[3]);
                    r.wind = std::stod(f[4]);
                    r.dir = std::stod(f[5]);
                    r.rain = std::stod(f[6]);
                    r.dew = std::stod(f[7]);
                    r.appt = std::stod(f[8]);
                    r.wetb = std::stod(f[9]);
                }
                records.push_back(r);
            }
            catch (...) {}
        }
        return true;
    }

    bool save() {
        backup();  // 先备份

        std::ofstream fout(filename);
        if (!fout) return false;
        fout << "timestamp,temp,hum,press,wind_max,dir,rain,dew,appt,wetb\n";
        for (auto& r : records) {
            fout << r.timestamp << ","
                << std::fixed << std::setprecision(1) << r.temp << ","
                << std::fixed << std::setprecision(0) << r.hum << ","
                << std::fixed << std::setprecision(2) << r.press << ","
                << std::fixed << std::setprecision(2) << r.wind << ","
                << std::fixed << std::setprecision(0) << r.dir << ","
                << std::fixed << std::setprecision(2) << r.rain << ","
                << std::fixed << std::setprecision(1) << r.dew << ","
                << std::fixed << std::setprecision(1) << r.appt << ","
                << std::fixed << std::setprecision(1) << r.wetb << "\n";
        }
        return true;
    }

    bool exportNewFormat(const std::string& outFile) {
        std::ofstream fout(outFile);
        if (!fout) return false;
        fout << "timestamp,temp,hum,press,wind_max,dir,rain,dew,appt,wetb\n";
        for (auto& r : records) {
            fout << r.timestamp << ","
                << std::fixed << std::setprecision(1) << r.temp << ","
                << std::fixed << std::setprecision(0) << r.hum << ","
                << std::fixed << std::setprecision(2) << r.press << ","
                << std::fixed << std::setprecision(2) << r.wind << ","
                << std::fixed << std::setprecision(0) << r.dir << ","
                << std::fixed << std::setprecision(2) << r.rain << ","
                << std::fixed << std::setprecision(1) << r.dew << ","
                << std::fixed << std::setprecision(1) << r.appt << ","
                << std::fixed << std::setprecision(1) << r.wetb << "\n";
        }
        return true;
    }

    void add(const Record& r) {
        records.push_back(r);
    }
};

// ==================== 美化输出 ====================
void printBanner() {
    std::cout << "\n";
    std::cout << C::BRIGHT_CYAN << "  +======================================================================+\n" << C::RESET;
    std::cout << C::BRIGHT_CYAN << "  |" << C::RESET << C::BOLD << C::BRIGHT_WHITE
        << "            自建气象站 · 数据管理系统  v1.0                          "
        << C::RESET << C::BRIGHT_CYAN << "|\n" << C::RESET;
    std::cout << C::BRIGHT_CYAN << "  |" << C::RESET << C::DIM
        << "            DIY Weather Station · Data Manager                        "
        << C::RESET << C::BRIGHT_CYAN << "|\n" << C::RESET;
    std::cout << C::BRIGHT_CYAN << "  +======================================================================+\n" << C::RESET;
    std::cout << "\n";
}

void printSection(const std::string& title, const std::string& icon = ">") {
    std::cout << "\n" << C::BRIGHT_CYAN << icon << " " << C::BOLD
        << C::BRIGHT_WHITE << title << C::RESET << "\n";
    std::cout << C::CYAN << "  " << repeat("-", 70) << C::RESET << "\n";
}

void printMenu() {
    std::cout << "\n";
    std::cout << C::BRIGHT_CYAN << "  +------------------------------------------------------------+\n" << C::RESET;
    std::cout << C::BRIGHT_CYAN << "  | " << C::RESET << C::BOLD << C::BRIGHT_WHITE
        << "                    主菜单                                  " << C::RESET << C::BRIGHT_CYAN << "|\n" << C::RESET;
    std::cout << C::BRIGHT_CYAN << "  +------------------------------------------------------------+\n" << C::RESET;
    std::cout << C::BRIGHT_CYAN << "  | " << C::RESET << C::BRIGHT_GREEN << " 1 " << C::RESET << C::WHITE
        << "输入数据" << C::RESET << C::DIM << "        手动添加一条气象记录         " << C::RESET << C::BRIGHT_CYAN << "|\n" << C::RESET;
    std::cout << C::BRIGHT_CYAN << "  | " << C::RESET << C::BRIGHT_GREEN << " 2 " << C::RESET << C::WHITE
        << "查询数据" << C::RESET << C::DIM << "        按日期/范围/条件查询           " << C::RESET << C::BRIGHT_CYAN << "|\n" << C::RESET;
    std::cout << C::BRIGHT_CYAN << "  | " << C::RESET << C::BRIGHT_GREEN << " 3 " << C::RESET << C::WHITE
        << "统计概览" << C::RESET << C::DIM << "        查看整体统计信息               " << C::RESET << C::BRIGHT_CYAN << "|\n" << C::RESET;
    std::cout << C::BRIGHT_CYAN << "  | " << C::RESET << C::BRIGHT_GREEN << " 4 " << C::RESET << C::WHITE
        << "保存文件" << C::RESET << C::DIM << "        写回 CSV                       " << C::RESET << C::BRIGHT_CYAN << "|\n" << C::RESET;
    std::cout << C::BRIGHT_CYAN << "  | " << C::RESET << C::BRIGHT_GREEN << " 5 " << C::RESET << C::WHITE
        << "重新加载" << C::RESET << C::DIM << "        从 CSV 重新读取                 " << C::RESET << C::BRIGHT_CYAN << "|\n" << C::RESET;
    std::cout << C::BRIGHT_CYAN << "  | " << C::RESET << C::BRIGHT_GREEN << " 6 " << C::RESET << C::WHITE
        << "格式转换" << C::RESET << C::DIM << "        旧 CSV 转新格式                  " << C::RESET << C::BRIGHT_CYAN << "|\n" << C::RESET;
    std::cout << C::BRIGHT_CYAN << "  | " << C::RESET << C::BRIGHT_RED << " 0 " << C::RESET << C::WHITE
        << "退出" << C::RESET << C::DIM << "            保存并退出                    " << C::RESET << C::BRIGHT_CYAN << "|\n" << C::RESET;
    std::cout << C::BRIGHT_CYAN << "  +------------------------------------------------------------+\n" << C::RESET;
    std::cout << "\n  " << C::DIM << "请选择 " << C::RESET << C::BRIGHT_YELLOW << "[0-6]" << C::RESET << C::DIM << " > " << C::RESET;
}

// ==================== 功能 1：输入数据 ====================
void featureInput(WeatherDB& db) {
    printSection("输入新数据", ">");
    std::cout << "  " << C::DIM << "提示：直接回车使用默认值" << C::RESET << "\n\n";

    Record r;
    std::string ts = inputString("时间 (YYYY-MM-DD HH:MM:SS)", nowTimestamp());
    r.timestamp = normalizeTimestamp(ts);
    r.date = extractDate(r.timestamp);

    r.temp = inputDouble("温度 (C)", 25.0);
    r.hum = inputDouble("湿度 (%)", 60.0);
    r.press = inputDouble("气压 (kPa)", 101.30);
    r.wind = inputDouble("风速 (m/s)", 0.0);
    r.dir = inputDouble("风向 (deg)", 0.0);
    r.rain = inputDouble("雨量 (mm)", 0.0);

    std::cout << "\n  " << C::DIM << "露点/体感/湿球可自动计算" << C::RESET << "\n";
    std::string a = inputString("是否自动计算? (y/n)", "y");
    if (a == "y" || a == "Y" || a.empty()) {
        double es = 0.61078 * exp((17.27 * r.temp) / (r.temp + 237.3));
        double e = es * r.hum / 100.0;
        r.dew = (237.3 * log(e / 0.61078)) / (17.27 - log(e / 0.61078));

        double e2 = r.hum / 100.0 * 6.105 * exp(17.27 * r.temp / (237.3 + r.temp));
        r.appt = r.temp + 0.33 * e2 - 0.70 * r.wind - 4.00;

        r.wetb = r.temp * atan(0.151977 * sqrt(r.hum + 8.313659))
            + atan(r.temp + r.hum)
            - atan(r.hum - 1.676331)
            + 0.00391838 * pow(r.hum, 1.5) * atan(0.023101 * r.hum)
            - 4.686035;

        std::cout << "\n  " << C::BRIGHT_GREEN << "[OK] 自动计算完成" << C::RESET << "\n";
        std::cout << "  " << C::DIM << "| " << C::RESET << C::WHITE << "露点: " << C::RESET << C::BRIGHT_CYAN << fmt(r.dew, 1) << "C" << C::RESET << "\n";
        std::cout << "  " << C::DIM << "| " << C::RESET << C::WHITE << "体感: " << C::RESET << C::BRIGHT_CYAN << fmt(r.appt, 1) << "C" << C::RESET << "\n";
        std::cout << "  " << C::DIM << "| " << C::RESET << C::WHITE << "湿球: " << C::RESET << C::BRIGHT_CYAN << fmt(r.wetb, 1) << "C" << C::RESET << "\n";
    }
    else {
        r.dew = inputDouble("露点 (C)", 0.0);
        r.appt = inputDouble("体感 (C)", 0.0);
        r.wetb = inputDouble("湿球 (C)", 0.0);
    }

    db.add(r);
    std::cout << "\n  " << C::BRIGHT_GREEN << "[OK] 已添加记录，当前共 " << db.records.size() << " 条" << C::RESET << "\n";
}

// ==================== 表格 ====================
void printTableHeader() {
    std::vector<std::string> h = { "时间", "温度", "湿度", "气压", "风速", "风向", "雨量", "露点", "体感", "湿球" };
    std::vector<int> w = { 20, 7, 6, 8, 6, 6, 7, 7, 7, 7 };
    std::cout << C::BRIGHT_CYAN << "  +";
    for (size_t i = 0; i < w.size(); i++) {
        std::cout << repeat("-", w[i] + 2) << "+";
    }
    std::cout << C::RESET << "\n  " << C::BRIGHT_CYAN << "| " << C::RESET;
    for (size_t i = 0; i < h.size(); i++) {
        padRight(C::BOLD + C::BRIGHT_WHITE + h[i] + C::RESET, w[i]);
        std::cout << " " << C::BRIGHT_CYAN << "| " << C::RESET;
    }
    std::cout << "\n" << C::BRIGHT_CYAN << "  +";
    for (size_t i = 0; i < w.size(); i++) {
        std::cout << repeat("-", w[i] + 2) << "+";
    }
    std::cout << C::RESET << "\n";
}

void printTableBottom() {
    std::vector<int> w = { 20, 7, 6, 8, 6, 6, 7, 7, 7, 7 };
    std::cout << C::BRIGHT_CYAN << "  +";
    for (size_t i = 0; i < w.size(); i++) {
        std::cout << repeat("-", w[i] + 2) << "+";
    }
    std::cout << C::RESET << "\n";
}

void printTableRow(const Record& r) {
    std::vector<int> w = { 20, 7, 6, 8, 6, 6, 7, 7, 7, 7 };
    std::cout << C::DIM << "  | " << C::RESET;
    padRight(C::CYAN + r.timestamp + C::RESET, w[0]);
    std::cout << " " << C::DIM << "| " << C::RESET;
    padRight(cTemp(r.temp) + fmt(r.temp, 1) + C::RESET, w[1]);
    std::cout << " " << C::DIM << "| " << C::RESET;
    padRight(cHum(r.hum) + fmt(r.hum, 0) + C::RESET, w[2]);
    std::cout << " " << C::DIM << "| " << C::RESET;
    padRight(cPress(r.press) + fmt(r.press, 2) + C::RESET, w[3]);
    std::cout << " " << C::DIM << "| " << C::RESET;
    padRight(cWind(r.wind) + fmt(r.wind, 2) + C::RESET, w[4]);
    std::cout << " " << C::DIM << "| " << C::RESET;
    padRight(fmt(r.dir, 0) + C::RESET, w[5]);
    std::cout << " " << C::DIM << "| " << C::RESET;
    padRight(cRain(r.rain) + fmt(r.rain, 2) + C::RESET, w[6]);
    std::cout << " " << C::DIM << "| " << C::RESET;
    double dd = r.temp - r.dew;
    padRight(cDewDiff(dd) + fmt(r.dew, 1) + C::RESET, w[7]);
    std::cout << " " << C::DIM << "| " << C::RESET;
    padRight(cTemp(r.appt) + fmt(r.appt, 1) + C::RESET, w[8]);
    std::cout << " " << C::DIM << "| " << C::RESET;
    padRight(cTemp(r.wetb) + fmt(r.wetb, 1) + C::RESET, w[9]);
    std::cout << " " << C::DIM << "|" << C::RESET << "\n";
}

// ==================== 功能 2：查询数据 ====================
void queryByDate(WeatherDB& db) {
    std::cout << "\n  " << C::DIM << "| " << C::RESET << C::WHITE
        << "输入日期 (YYYY-MM-DD): " << C::RESET;
    std::string date;
    std::getline(std::cin, date);
    if (date.empty()) return;

    std::vector<Record> hits;
    for (auto& r : db.records) if (r.date == date) hits.push_back(r);

    std::cout << "\n  " << C::BRIGHT_CYAN << "> 查询结果: " << C::RESET
        << C::BRIGHT_WHITE << date << C::RESET
        << C::DIM << "  命中 " << C::RESET << C::BRIGHT_YELLOW << hits.size() << C::RESET << C::DIM << " 条" << C::RESET << "\n";
    if (hits.empty()) {
        std::cout << "  " << C::BRIGHT_RED << "[X] 无匹配记录" << C::RESET << "\n";
        return;
    }
    std::cout << "\n";
    printTableHeader();
    for (auto& r : hits) printTableRow(r);
    printTableBottom();
}

void queryByRange(WeatherDB& db) {
    std::cout << "\n  " << C::DIM << "| " << C::RESET << C::WHITE << "起始日期 (YYYY-MM-DD): " << C::RESET;
    std::string d1; std::getline(std::cin, d1);
    std::cout << "  " << C::DIM << "| " << C::RESET << C::WHITE << "结束日期 (YYYY-MM-DD): " << C::RESET;
    std::string d2; std::getline(std::cin, d2);
    if (d1.empty() || d2.empty()) return;
    if (d1 > d2) std::swap(d1, d2);

    std::vector<Record> hits;
    for (auto& r : db.records) if (r.date >= d1 && r.date <= d2) hits.push_back(r);

    std::cout << "\n  " << C::BRIGHT_CYAN << "> 查询范围: " << C::RESET
        << C::BRIGHT_WHITE << d1 << " ~ " << d2 << C::RESET
        << C::DIM << "  命中 " << C::RESET << C::BRIGHT_YELLOW << hits.size() << C::RESET << C::DIM << " 条" << C::RESET << "\n";
    if (hits.empty()) {
        std::cout << "  " << C::BRIGHT_RED << "[X] 无匹配记录" << C::RESET << "\n";
        return;
    }
    std::cout << "\n";
    printTableHeader();
    for (auto& r : hits) printTableRow(r);
    printTableBottom();
}

void queryByCondition(WeatherDB& db) {
    std::cout << "\n  " << C::DIM << "可选条件：温度/湿度/气压/风速/雨量/露点" << C::RESET << "\n";
    std::cout << "  " << C::DIM << "| " << C::RESET << C::WHITE << "字段: " << C::RESET;
    std::string field; std::getline(std::cin, field);
    if (field.empty()) return;

    std::cout << "  " << C::DIM << "| " << C::RESET << C::WHITE << "运算符 (>, <, >=, <=, ==): " << C::RESET;
    std::string op; std::getline(std::cin, op);
    std::cout << "  " << C::DIM << "| " << C::RESET << C::WHITE << "阈值: " << C::RESET;
    std::string vstr; std::getline(std::cin, vstr);
    double v = 0;
    try { v = std::stod(vstr); }
    catch (...) {
        std::cout << "  " << C::BRIGHT_RED << "[X] 无效数字" << C::RESET << "\n";
        return;
    }

    auto getVal = [&](const Record& r) -> double {
        if (field == "温度" || field == "temp")   return r.temp;
        if (field == "湿度" || field == "hum")    return r.hum;
        if (field == "气压" || field == "press")  return r.press;
        if (field == "风速" || field == "wind")   return r.wind;
        if (field == "雨量" || field == "rain")   return r.rain;
        if (field == "露点" || field == "dew")    return r.dew;
        return NAN;
        };

    auto match = [&](double x) -> bool {
        if (op == ">")  return x > v;
        if (op == "<")  return x < v;
        if (op == ">=") return x >= v;
        if (op == "<=") return x <= v;
        if (op == "==") return std::fabs(x - v) < 1e-9;
        return false;
        };

    std::vector<Record> hits;
    for (auto& r : db.records) {
        double x = getVal(r);
        if (std::isnan(x)) break;
        if (match(x)) hits.push_back(r);
    }

    std::cout << "\n  " << C::BRIGHT_CYAN << "> 查询条件: " << C::RESET
        << C::BRIGHT_WHITE << field << " " << op << " " << v << C::RESET
        << C::DIM << "  命中 " << C::RESET << C::BRIGHT_YELLOW << hits.size() << C::RESET << C::DIM << " 条" << C::RESET << "\n";
    if (hits.empty()) {
        std::cout << "  " << C::BRIGHT_RED << "[X] 无匹配记录" << C::RESET << "\n";
        return;
    }
    std::cout << "\n";
    printTableHeader();
    for (auto& r : hits) printTableRow(r);
    printTableBottom();
}

void queryLatest(WeatherDB& db) {
    int n = 20;
    std::cout << "\n  " << C::DIM << "| " << C::RESET << C::WHITE
        << "显示最近多少条? [20]: " << C::RESET;
    std::string s; std::getline(std::cin, s);
    if (!s.empty()) { try { n = std::stoi(s); } catch (...) {} }

    int total = (int)db.records.size();
    int start = (std::max)(0, total - n);
    std::cout << "\n  " << C::BRIGHT_CYAN << "> 最近 " << (total - start) << " 条记录" << C::RESET << "\n\n";
    printTableHeader();
    for (int i = start; i < total; i++) printTableRow(db.records[i]);
    printTableBottom();
}

void featureQuery(WeatherDB& db) {
    printSection("查询数据", "[?]");
    std::cout << "  " << C::BRIGHT_GREEN << " 1 " << C::RESET << "按日期查询\n";
    std::cout << "  " << C::BRIGHT_GREEN << " 2 " << C::RESET << "按日期范围查询\n";
    std::cout << "  " << C::BRIGHT_GREEN << " 3 " << C::RESET << "按条件查询 (温度/湿度/...)\n";
    std::cout << "  " << C::BRIGHT_GREEN << " 4 " << C::RESET << "查看最近记录\n";
    std::cout << "  " << C::BRIGHT_GREEN << " 0 " << C::RESET << "返回\n";
    std::cout << "\n  " << C::DIM << "请选择 > " << C::RESET;
    std::string ch; std::getline(std::cin, ch);
    if (ch == "1") queryByDate(db);
    else if (ch == "2") queryByRange(db);
    else if (ch == "3") queryByCondition(db);
    else if (ch == "4") queryLatest(db);
}

// ==================== 功能 3：统计概览 ====================
void featureStats(WeatherDB& db) {
    printSection("统计概览", "[#]");
    if (db.records.empty()) {
        std::cout << "  " << C::DIM << "暂无数据" << C::RESET << "\n";
        return;
    }

    double tmax = -999, tmin = 999, tsum = 0;
    double humMax = -999, humMin = 999, humSum = 0;
    double pMax = -999, pMin = 999, pSum = 0;
    double rSum = 0;
    double dewMinDiff = 999;
    int calm = 0;
    std::string tMaxDate, tMinDate;

    for (auto& r : db.records) {
        if (r.temp > tmax) { tmax = r.temp; tMaxDate = r.timestamp; }
        if (r.temp < tmin) { tmin = r.temp; tMinDate = r.timestamp; }
        tsum += r.temp;
        if (r.hum > humMax) humMax = r.hum;
        if (r.hum < humMin) humMin = r.hum;
        humSum += r.hum;
        if (r.press > pMax) pMax = r.press;
        if (r.press < pMin) pMin = r.press;
        pSum += r.press;
        rSum += r.rain;
        double dd = r.temp - r.dew;
        if (dd < dewMinDiff) dewMinDiff = dd;
        if (r.wind < 0.01) calm++;
    }
    int n = (int)db.records.size();

    std::cout << "\n  " << C::BOLD << C::BRIGHT_WHITE << "> 温度" << C::RESET << "\n";
    std::cout << "  " << C::DIM << "| " << C::RESET << C::WHITE << "最高   " << C::RESET
        << C::BRIGHT_RED << fmt(tmax, 1) << "C" << C::RESET << C::DIM << "  " << tMaxDate << C::RESET << "\n";
    std::cout << "  " << C::DIM << "| " << C::RESET << C::WHITE << "最低   " << C::RESET
        << C::BRIGHT_CYAN << fmt(tmin, 1) << "C" << C::RESET << C::DIM << "  " << tMinDate << C::RESET << "\n";
    std::cout << "  " << C::DIM << "| " << C::RESET << C::WHITE << "平均   " << C::RESET
        << C::YELLOW << fmt(tsum / n, 1) << "C" << C::RESET << "\n";

    std::cout << "\n  " << C::BOLD << C::BRIGHT_WHITE << "> 湿度" << C::RESET << "\n";
    std::cout << "  " << C::DIM << "| " << C::RESET << C::WHITE << "最高   " << C::RESET
        << C::BRIGHT_BLUE << fmt(humMax, 0) << "%" << C::RESET << "\n";
    std::cout << "  " << C::DIM << "| " << C::RESET << C::WHITE << "最低   " << C::RESET
        << C::YELLOW << fmt(humMin, 0) << "%" << C::RESET << "\n";
    std::cout << "  " << C::DIM << "| " << C::RESET << C::WHITE << "平均   " << C::RESET
        << C::BLUE << fmt(humSum / n, 0) << "%" << C::RESET << "\n";

    std::cout << "\n  " << C::BOLD << C::BRIGHT_WHITE << "> 气压" << C::RESET << "\n";
    std::cout << "  " << C::DIM << "| " << C::RESET << C::WHITE << "范围   " << C::RESET
        << C::BRIGHT_CYAN << fmt(pMin, 2) << " ~ " << fmt(pMax, 2) << " kPa" << C::RESET << "\n";
    std::cout << "  " << C::DIM << "| " << C::RESET << C::WHITE << "平均   " << C::RESET
        << C::CYAN << fmt(pSum / n, 2) << " kPa" << C::RESET << "\n";

    std::cout << "\n  " << C::BOLD << C::BRIGHT_WHITE << "> 其他" << C::RESET << "\n";
    std::cout << "  " << C::DIM << "| " << C::RESET << C::WHITE << "总雨量 " << C::RESET
        << C::BRIGHT_BLUE << fmt(rSum, 2) << " mm" << C::RESET << "\n";
    std::cout << "  " << C::DIM << "| " << C::RESET << C::WHITE << "静风率 " << C::RESET
        << C::MAGENTA << fmt(100.0 * calm / n, 1) << "%" << C::RESET
        << C::DIM << "  (" << calm << "/" << n << ")" << C::RESET << "\n";
    std::cout << "  " << C::DIM << "| " << C::RESET << C::WHITE << "露点差 " << C::RESET
        << cDewDiff(dewMinDiff) << "最小 " << fmt(dewMinDiff, 2) << "C" << C::RESET << "\n";
}

// ==================== 功能 6：格式转换 ====================
void featureConvert(WeatherDB& db) {
    printSection("格式转换", "[<>]");
    if (db.records.empty()) {
        std::cout << "  " << C::DIM << "暂无数据" << C::RESET << "\n";
        return;
    }
    std::cout << "  " << C::DIM << "| " << C::RESET << C::WHITE
        << "源文件格式: " << C::RESET
        << (db.lastLoadWasOldFormat ? C::BRIGHT_YELLOW + "旧格式" : C::BRIGHT_GREEN + "新格式")
        << C::RESET << "\n";
    std::cout << "  " << C::DIM << "| " << C::RESET << C::WHITE
        << "记录数: " << C::RESET << C::BRIGHT_YELLOW << db.records.size() << C::RESET << "\n";

    std::string outFile = inputString("输出文件名", "data_new.csv");
    if (db.exportNewFormat(outFile)) {
        std::cout << "\n  " << C::BRIGHT_GREEN << "[OK] 已导出到 " << outFile << C::RESET << "\n";
    }
    else {
        std::cout << "\n  " << C::BRIGHT_RED << "[X] 导出失败" << C::RESET << "\n";
    }
}

// ==================== 主程序 ====================
int main(int argc, char** argv) {
    enableAnsi();

    // 打印当前工作目录，方便排查数据文件位置
    std::cout << C::DIM << "当前工作目录: " << C::RESET << getCwd() << "\n";

    // 相对路径：data.csv 位于当前工作目录
    std::string filename = (argc > 1) ? argv[1] : "data.csv";

    WeatherDB db(filename);
    db.load();

    printBanner();
    std::cout << "  " << C::DIM << "数据文件: " << C::RESET << C::BRIGHT_WHITE << filename << C::RESET;
    std::cout << C::DIM << "   已加载 " << C::RESET << C::BRIGHT_YELLOW << db.records.size() << C::RESET << C::DIM << " 条记录" << C::RESET;
    if (db.lastLoadWasOldFormat) {
        std::cout << C::DIM << "  [" << C::RESET << C::BRIGHT_YELLOW << "旧格式" << C::RESET << C::DIM << "]" << C::RESET;
    }
    else {
        std::cout << C::DIM << "  [" << C::RESET << C::BRIGHT_GREEN << "新格式" << C::RESET << C::DIM << "]" << C::RESET;
    }
    std::cout << "\n";

    while (true) {
        printMenu();
        std::string choice;
        std::getline(std::cin, choice);

        if (choice == "0") {
            std::cout << "\n  " << C::DIM << "保存中..." << C::RESET << "\n";
            if (db.save()) std::cout << "  " << C::BRIGHT_GREEN << "[OK] 已保存到 " << filename << C::RESET << "\n";
            std::cout << "\n  " << C::BRIGHT_CYAN << "再见 ~" << C::RESET << "\n\n";
            break;
        }
        else if (choice == "1") featureInput(db);
        else if (choice == "2") featureQuery(db);
        else if (choice == "3") featureStats(db);
        else if (choice == "4") {
            if (db.save()) std::cout << "\n  " << C::BRIGHT_GREEN << "[OK] 已保存 " << db.records.size() << " 条到 " << filename << C::RESET << "\n";
            else          std::cout << "\n  " << C::BRIGHT_RED << "[X] 保存失败" << C::RESET << "\n";
        }
        else if (choice == "5") {
            db.records.clear();
            db.load();
            std::cout << "\n  " << C::BRIGHT_GREEN << "[OK] 已重新加载 " << db.records.size() << " 条" << C::RESET << "\n";
        }
        else if (choice == "6") featureConvert(db);
        else {
            std::cout << "\n  " << C::BRIGHT_RED << "[X] 无效选项" << C::RESET << "\n";
        }
    }
    return 0;
}
