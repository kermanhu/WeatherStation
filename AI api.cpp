#define _CRT_SECURE_NO_WARNINGS
#pragma comment(lib, "winhttp.lib")

#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <thread>
#include <windows.h>
#include <winhttp.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

std::string getCurrentTimeStr() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm bt;
    localtime_s(&bt, &in_time_t);
    std::ostringstream oss;
    oss << std::put_time(&bt, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

std::string getDesktopPath() {
    const char* home = std::getenv("USERPROFILE");
    if (home) return std::string(home) + "\\Desktop\\";
    return "./";
}

// ---------- 读取完整历史 CSV ----------
std::string readHistoryCSV() {
    std::string histFile = "history_data.csv";
    std::ifstream ifs(histFile, std::ios::in | std::ios::binary);
    if (!ifs.is_open()) {
        std::cerr << "[历史] 无法打开 " << histFile << "，将不使用历史数据。" << std::endl;
        return "";
    }
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ifs.close();
    std::cout << "[历史] 已读取 CSV，共 " << content.size() << " 字节，"
        << std::count(content.begin(), content.end(), '\n') << " 行。" << std::endl;
    return content;
}

// ---------- 获取气象数据（实时） ----------
std::string fetchWeatherData() {
    std::string tmpFile = "curl_temp.txt";
    std::string cmd = "curl -s -m 5 (Weather Station IP) > \"" + tmpFile + "\" 2>&1";
    system(cmd.c_str());
    std::ifstream ifs(tmpFile);
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ifs.close();
    std::remove(tmpFile.c_str());
    return content;
}

// ---------- 获取历史数据（/csv 接口，带重试） ----------
std::string fetchHistoryData() {
    const int MAX_RETRIES = 5;
    const int CONNECT_TIMEOUT = 30;
    const int MAX_TIME = 60;
    const int RETRY_DELAY_SEC = 3;

    for (int attempt = 1; attempt <= MAX_RETRIES; ++attempt) {
        std::cout << "[历史] 正在获取历史数据 (尝试 " << attempt << "/" << MAX_RETRIES << ") ..." << std::endl;

        std::string tmpFile = "curl_history_temp.txt";
        std::string errFile = "curl_history_err.txt";
        std::string cmd = "curl -v --no-buffer --connect-timeout " + std::to_string(CONNECT_TIMEOUT) +
            " --max-time " + std::to_string(MAX_TIME) +
            " -H \"Connection: close\" " +
            " http://192.168.1.24/csv > \"" + tmpFile + "\" 2> \"" + errFile + "\"";

        int ret = system(cmd.c_str());
        std::cout << "[历史] curl 返回值: " << ret << std::endl;

        std::ifstream errIfs(errFile);
        if (errIfs.is_open()) {
            std::string errContent((std::istreambuf_iterator<char>(errIfs)), std::istreambuf_iterator<char>());
            errIfs.close();
            if (!errContent.empty()) {
                std::cerr << "[历史] curl 错误信息: " << errContent << std::endl;
            }
        }
        std::remove(errFile.c_str());

        std::ifstream ifs(tmpFile);
        if (!ifs.is_open()) {
            std::cerr << "[历史] 尝试 " << attempt << " 失败：无法打开临时文件" << std::endl;
            std::remove(tmpFile.c_str());
            if (attempt < MAX_RETRIES) {
                std::this_thread::sleep_for(std::chrono::seconds(RETRY_DELAY_SEC));
                continue;
            }
            return "";
        }

        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        ifs.close();
        std::remove(tmpFile.c_str());

        if (!content.empty() && content.find('\n') != std::string::npos) {
            std::cout << "[历史] 获取成功！共 " << std::count(content.begin(), content.end(), '\n') << " 行" << std::endl;
            return content;
        }
        else {
            std::cerr << "[历史] 尝试 " << attempt << " 失败：返回内容为空或格式无效" << std::endl;
            if (attempt < MAX_RETRIES) {
                std::this_thread::sleep_for(std::chrono::seconds(RETRY_DELAY_SEC));
            }
        }
    }

    std::cerr << "[历史] 所有重试均失败，放弃获取历史数据。" << std::endl;
    return "";
}

// ---------- 调用 DeepSeek API ----------
std::string callDeepSeek(const std::string& apiKey, const json& messages) {
    json requestBody = {
        {"model", "deepseek-v4-flash"},
        {"messages", messages},
        {"temperature", 1.0},
        {"max_tokens", 8192}
    };

    std::string postData = requestBody.dump();
    std::string result;

    HINTERNET hSession = WinHttpOpen(L"Test", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
    if (!hSession) {
        std::cerr << "[WinHTTP] WinHttpOpen 失败，错误码: " << GetLastError() << std::endl;
        return "";
    }

    DWORD timeout = 60000;
    WinHttpSetOption(hSession, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    WinHttpSetOption(hSession, WINHTTP_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));
    WinHttpSetOption(hSession, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    DWORD tlsProtocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    WinHttpSetOption(hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &tlsProtocols, sizeof(tlsProtocols));

    HINTERNET hConnect = WinHttpConnect(hSession, L"api.deepseek.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        std::cerr << "[WinHTTP] WinHttpConnect 失败，错误码: " << GetLastError() << std::endl;
        WinHttpCloseHandle(hSession);
        return "";
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", L"/v1/chat/completions",
        NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        std::cerr << "[WinHTTP] WinHttpOpenRequest 失败，错误码: " << GetLastError() << std::endl;
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    DWORD securityFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
        SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
        SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &securityFlags, sizeof(securityFlags));

    std::wstring headers = L"Content-Type: application/json; charset=utf-8\r\n"
        L"Accept: application/json\r\n"
        L"Authorization: Bearer " + std::wstring(apiKey.begin(), apiKey.end()) + L"\r\n";

    if (!WinHttpSendRequest(hRequest, headers.c_str(), (DWORD)headers.length(),
        (LPVOID)postData.c_str(), (DWORD)postData.size(),
        (DWORD)postData.size(), 0)) {
        std::cerr << "[WinHTTP] WinHttpSendRequest 失败，错误码: " << GetLastError() << std::endl;
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        std::cerr << "[WinHTTP] WinHttpReceiveResponse 失败，错误码: " << GetLastError() << std::endl;
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        NULL, &statusCode, &statusCodeSize, NULL);
    std::cerr << "[WinHTTP] HTTP 状态码: " << statusCode << std::endl;

    if (statusCode != 200) {
        DWORD bytesRead = 0;
        char buffer[4096];
        while (WinHttpReadData(hRequest, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0) {
            buffer[bytesRead] = '\0';
            result.append(buffer, bytesRead);
        }
        std::cerr << "[WinHTTP] 错误响应: " << result << std::endl;
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    DWORD bytesRead = 0;
    char buffer[4096];
    while (WinHttpReadData(hRequest, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        result.append(buffer, bytesRead);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    std::string debugFile = getDesktopPath() + "debug_response.txt";
    std::ofstream debugOut(debugFile, std::ios::binary);
    if (debugOut.is_open()) {
        debugOut << result;
        debugOut.close();
    }

    if (result.empty()) {
        std::cerr << "[WinHTTP] 响应为空" << std::endl;
        return "";
    }

    try {
        auto respJson = json::parse(result);
        if (respJson.contains("choices") && !respJson["choices"].empty()) {
            return respJson["choices"][0]["message"]["content"].get<std::string>();
        }
    }
    catch (const std::exception& e) {
        std::cerr << "[WinHTTP] JSON解析失败: " << e.what() << std::endl;
        std::cerr << "完整响应: " << result << std::endl;
    }
    return "";
}

// UTF-8 转 GB2312
std::string utf8ToGB2312(const std::string& utf8Str) {
    if (utf8Str.empty()) return "";
    int wLen = MultiByteToWideChar(CP_UTF8, 0, utf8Str.c_str(), -1, NULL, 0);
    if (wLen == 0) return utf8Str;
    std::wstring wStr(wLen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8Str.c_str(), -1, &wStr[0], wLen);
    int gbLen = WideCharToMultiByte(CP_ACP, 0, wStr.c_str(), -1, NULL, 0, NULL, NULL);
    if (gbLen == 0) return utf8Str;
    std::string gbStr(gbLen, '\0');
    WideCharToMultiByte(CP_ACP, 0, wStr.c_str(), -1, &gbStr[0], gbLen, NULL, NULL);
    if (!gbStr.empty() && gbStr.back() == '\0') gbStr.pop_back();
    return gbStr;
}

// ---------- 生成 HTML ----------
void generateHTML(const json& data, const std::string& aiConclusion, const std::string& timeStr) {
    std::string filename = "实时预报.html";
    std::ofstream file(filename, std::ios::out | std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "无法创建 HTML 文件" << std::endl;
        return;
    }

    double temp = data.value("temp", 0.0);
    double hum = data.value("hum", 0.0);
    double press = data.value("press", 0.0);
    double wind = data.value("wind", 0.0);
    double dir = data.value("dir", 0.0);
    double rain = data.value("rain", 0.0);
    double dew = data.value("dew", 0.0);
    double appt = data.value("appt", 0.0);
    double wetb = data.value("wetb", 0.0);
    std::string boot = data.value("boot", "N/A");

    std::string gbAi = utf8ToGB2312(aiConclusion);

    std::stringstream html;
    html << R"HTML(<!DOCTYPE html>
<html>
<head>
    <meta charset="GB2312">
    <title>气象站实时报告</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: 'Microsoft YaHei', 'PingFang SC', Arial, sans-serif;
            background: #0a0e1a;
            color: #e8edf5;
            padding: 20px;
            display: flex;
            justify-content: center;
            min-height: 100vh;
        }
        .container {
            max-width: 960px;
            width: 100%;
            background: #111827;
            border-radius: 24px;
            padding: 30px 35px 40px;
            border: 1px solid #1e3a5f;
            box-shadow: 0 20px 60px rgba(0,20,80,0.5);
        }
        h1 {
            font-size: 28px;
            color: #e8edf5;
            text-align: center;
            margin-bottom: 4px;
            letter-spacing: 2px;
        }
        .subtitle {
            text-align: center;
            color: #6b8ab0;
            font-size: 14px;
            margin-bottom: 25px;
        }
        .grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(140px, 1fr));
            gap: 14px;
            margin-bottom: 30px;
        }
        .card {
            background: #0d1425;
            border-radius: 12px;
            padding: 14px 10px;
            text-align: center;
            border: 1px solid #1a2a4a;
            transition: 0.2s;
        }
        .card:hover {
            border-color: #3b82f6;
            box-shadow: 0 0 20px rgba(59,130,246,0.05);
        }
        .card .label {
            font-size: 11px;
            color: #6b8ab0;
            text-transform: uppercase;
            letter-spacing: 0.5px;
        }
        .card .value {
            font-size: 22px;
            font-weight: 700;
            color: #e8edf5;
            margin-top: 4px;
        }
        .card .unit {
            font-size: 13px;
            color: #6b8ab0;
            margin-left: 2px;
        }
        .section-title {
            font-size: 18px;
            font-weight: 600;
            color: #e8edf5;
            margin: 20px 0 14px 0;
            padding-bottom: 10px;
            border-bottom: 2px solid #1a2a4a;
            display: flex;
            justify-content: space-between;
            align-items: center;
            flex-wrap: wrap;
            gap: 8px;
        }
        .badge {
            background: #1e3a5f;
            color: #93c5fd;
            font-size: 11px;
            padding: 2px 14px;
            border-radius: 20px;
        }
        .ai-box {
            background: #0d1425;
            border-left: 4px solid #3b82f6;
            border-radius: 8px;
            padding: 18px 22px;
            min-height: 80px;
            line-height: 1.9;
            color: #cbd5e1;
            white-space: pre-wrap;
            word-wrap: break-word;
            font-size: 14px;
        }
        .footer {
            margin-top: 25px;
            padding-top: 14px;
            border-top: 1px solid #1a2a4a;
            font-size: 11px;
            color: #4a6a8a;
            text-align: center;
        }
        @media (max-width: 600px) {
            .container { padding: 18px; }
            .grid { grid-template-columns: repeat(3, 1fr); gap: 10px; }
            .card .value { font-size: 18px; }
        }
        @media (max-width: 400px) {
            .grid { grid-template-columns: repeat(2, 1fr); }
        }
    </style>
</head>
<body>
<div class="container">
    <h1>气象站实时报告</h1>
    <div class="subtitle">观测时间：)HTML" << timeStr << R"HTML(</div>

    <div class="grid">
        <div class="card"><div class="label">温度</div><div class="value">)HTML" << temp << R"HTML(<span class="unit">℃</span></div></div>
        <div class="card"><div class="label">湿度</div><div class="value">)HTML" << hum << R"HTML(<span class="unit">%</span></div></div>
        <div class="card"><div class="label">气压</div><div class="value">)HTML" << press << R"HTML(<span class="unit">kPa</span></div></div>
        <div class="card"><div class="label">风速</div><div class="value">)HTML" << wind << R"HTML(<span class="unit">m/s</span></div></div>
        <div class="card"><div class="label">风向</div><div class="value">)HTML" << dir << R"HTML(<span class="unit">°</span></div></div>
        <div class="card"><div class="label">雨量</div><div class="value">)HTML" << rain << R"HTML(<span class="unit">mm</span></div></div>
        <div class="card"><div class="label">露点</div><div class="value">)HTML" << dew << R"HTML(<span class="unit">℃</span></div></div>
        <div class="card"><div class="label">体感</div><div class="value">)HTML" << appt << R"HTML(<span class="unit">℃</span></div></div>
        <div class="card"><div class="label">湿球</div><div class="value">)HTML" << wetb << R"HTML(<span class="unit">℃</span></div></div>
    </div>

    <div class="section-title">
        <span>AI 分析</span>
        <span class="badge">DeepSeek V4</span>
    </div>

    <div class="ai-box">)HTML" << gbAi << R"HTML(</div>

    <div class="footer">设备开机时间：)HTML" << boot << R"HTML( · 报告生成：)HTML" << getCurrentTimeStr() << R"HTML(</div>
</div>
</body>
</html>
)HTML";

    file << html.str();
    file.close();
    std::cout << "[OK] HTML 报告已保存至: " << filename << std::endl;
}

int main() {
    try {
        // 第一步：获取历史数据（保留在内存，不落盘）
        std::cout << "\n========== 获取历史数据 ==========" << std::endl;
        std::string history = fetchHistoryData();
        if (history.empty()) {
            std::cerr << "[历史] 获取历史数据失败，将跳过此步骤继续运行。" << std::endl;
        }
        else {
            std::cout << "[历史] 已获取历史数据（内存中保留，供每轮 AI 分析使用）" << std::endl;
        }

        // 第二步：AI 分析主循环
        const char* apiKey = **************************;
        if (!apiKey || strlen(apiKey) == 0) {
            std::cerr << "[警告] 未设置 API Key，AI 功能禁用。" << std::endl;
            return 1;
        }

        while (true) {
            json messages = json::array({
                {{"role", "system"}, {"content", "You are a professional weather data analyst. Please respond in Chinese. Use plain text only, no Markdown."}}
                });

            std::cout << "\n========== 开始获取数据 (" << getCurrentTimeStr() << ") ==========" << std::endl;
            std::string response = fetchWeatherData();
            if (response.empty()) {
                std::cerr << "[错误] 获取数据失败，10秒后重试..." << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(10));
                continue;
            }

            json data;
            try {
                data = json::parse(response);
            }
            catch (const std::exception& e) {
                std::cerr << "[错误] JSON 解析失败: " << e.what() << std::endl;
                std::this_thread::sleep_for(std::chrono::minutes(10));
                continue;
            }

            std::string timeStr = getCurrentTimeStr();

            // 复用内存中的历史数据
            std::string historyCSV = history;

            std::stringstream prompt_en;
            prompt_en << "=== FULL HISTORY DATA (CSV) ===\n";
            prompt_en << "Columns: temperature, humidity, pressure, wind speed, wind direction, precipitation, dew point, apparent temperature, wet bulb temperature\n";
            if (!historyCSV.empty()) {
                prompt_en << historyCSV << "\n";
            }
            else {
                prompt_en << "(history data unavailable)\n";
            }
            prompt_en << "=== CURRENT OBSERVATION ===\n";
            prompt_en << "Observation time: " << timeStr << "\n";
            prompt_en << "Temperature: " << data.value("temp", 0.0) << " C\n";
            prompt_en << "Humidity: " << data.value("hum", 0.0) << " %\n";
            prompt_en << "Pressure: " << data.value("press", 0.0) << " kPa\n";
            prompt_en << "Wind Speed: " << data.value("wind", 0.0) << " m/s\n";
            prompt_en << "Wind Direction: " << data.value("dir", 0.0) << " deg\n";
            prompt_en << "Rainfall: " << data.value("rain", 0.0) << " mm\n";
            prompt_en << "Dew Point: " << data.value("dew", 0.0) << " C\n";
            prompt_en << "Apparent Temperature: " << data.value("appt", 0.0) << " C\n";
            prompt_en << "Wet Bulb Temperature: " << data.value("wetb", 0.0) << " C\n";
            prompt_en << "\nPlease analyze based on the FULL history above and provide:\n";
            prompt_en << "1) Current weather status\n";
            prompt_en << "2) Trend comparison with recent days\n";
            prompt_en << "3) Brief weather outlook.\n";
            prompt_en << "Respond in Chinese, plain text only.";

            json userMsg = { {"role", "user"}, {"content", prompt_en.str()} };
            messages.push_back(userMsg);

            std::cout << "[DeepSeek] 正在调用 API（messages 共 " << messages.size() << " 条，prompt 约 "
                << prompt_en.str().size() << " 字节）..." << std::endl;

            std::string aiResponse = callDeepSeek(apiKey, messages);
            if (aiResponse.empty()) {
                aiResponse = "【AI 分析暂时不可用】请检查网络或 API Key 是否有效。";
                std::cerr << "[DeepSeek] 调用失败。" << std::endl;
            }
            else {
                std::cout << "[DeepSeek] 收到响应。" << std::endl;
            }

            generateHTML(data, aiResponse, timeStr);

            std::cout << "等待 10 分钟后进行下一次查询... (按 Ctrl+C 退出)" << std::endl;
            std::this_thread::sleep_for(std::chrono::minutes(10));
        }
    }
    catch (const std::exception& e) {
        std::cerr << "[致命错误] " << e.what() << std::endl;
        std::cin.get();
        return 1;
    }
    catch (...) {
        std::cerr << "[未知异常]" << std::endl;
        std::cin.get();
        return 1;
    }
}