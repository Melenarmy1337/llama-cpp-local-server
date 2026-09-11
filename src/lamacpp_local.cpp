#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")

#include <tlhelp32.h>

namespace fs = std::filesystem;

struct Settings {
    std::string llama_host = "0.0.0.0";
    int llama_port = 8080;
    std::string proxy_host = "0.0.0.0";
    int proxy_port = 11434;
    std::vector<int> contexts = {262144, 196608, 131072, 98304, 65536, 32768, 16384, 8192};
    int threads = 8;
    int threads_batch = 8;
    int parallel = 1;
    int fit_target_mib = 512;
    int batch_size = 2048;
    int ubatch_size = 512;
    int gpu_layers = -1;
    bool fit = true;
    bool no_host = false;
    bool kv_offload = false;
    std::string cache_type_k = "q8_0";
    std::string cache_type_v = "q8_0";
    std::string profile = "long";
};

static Settings settings_for_profile(const std::string & profile) {
    Settings s;
    if (profile == "fast") {
        s.contexts = {8192};
        s.threads = 16;
        s.threads_batch = 16;
        s.fit_target_mib = 512;
        s.fit = true;
        s.no_host = false;
        s.kv_offload = true;
        s.cache_type_k = "q4_0";
        s.cache_type_v = "q4_0";
        s.profile = "fast";
    } else if (profile == "balanced") {
        s.contexts = {32768, 16384, 8192};
        s.threads = 16;
        s.threads_batch = 16;
        s.fit_target_mib = 512;
        s.kv_offload = true;
        s.cache_type_k = "q4_0";
        s.cache_type_v = "q4_0";
        s.profile = "balanced";
    } else if (profile == "long") {
        s.fit_target_mib = 2048;
        s.kv_offload = false;
        s.profile = "long";
    } else {
        throw std::runtime_error("unknown server profile: " + profile);
    }
    return s;
}

struct HttpResponse {
    int status = 0;
    std::string raw;
    std::string headers;
    std::string body;
};

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

static std::string trim(const std::string & s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

static bool parse_hex4(const std::string & s, size_t pos, uint32_t & value) {
    if (pos + 4 > s.size()) return false;
    value = 0;
    for (size_t i = 0; i < 4; ++i) {
        int v = hex_value(s[pos + i]);
        if (v < 0) return false;
        value = (value << 4) | uint32_t(v);
    }
    return true;
}

static void append_utf8(std::string & out, uint32_t cp) {
    if (cp <= 0x7F) {
        out += char(cp);
    } else if (cp <= 0x7FF) {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    } else {
        out += char(0xF0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3F));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
}

static std::string decode_chunked(const std::string & body) {
    std::string out;
    size_t pos = 0;
    for (;;) {
        size_t line_end = body.find("\r\n", pos);
        if (line_end == std::string::npos) return body;
        std::string size_text = body.substr(pos, line_end - pos);
        size_t semi = size_text.find(';');
        if (semi != std::string::npos) size_text.resize(semi);
        size_text = trim(size_text);
        size_t chunk_size = 0;
        try {
            chunk_size = std::stoull(size_text, nullptr, 16);
        } catch (...) {
            return body;
        }
        pos = line_end + 2;
        if (chunk_size == 0) break;
        if (pos + chunk_size > body.size()) return body;
        out.append(body, pos, chunk_size);
        pos += chunk_size;
        if (pos + 2 <= body.size() && body.substr(pos, 2) == "\r\n") pos += 2;
    }
    return out;
}

static std::string quote_arg(const std::string & s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else out += c;
    }
    out += "\"";
    return out;
}

static fs::path exe_path() {
    std::string buffer(MAX_PATH, '\0');
    DWORD len = GetModuleFileNameA(nullptr, buffer.data(), DWORD(buffer.size()));
    if (len == 0) throw std::runtime_error("GetModuleFileNameA failed");
    while (len == buffer.size()) {
        buffer.resize(buffer.size() * 2, '\0');
        len = GetModuleFileNameA(nullptr, buffer.data(), DWORD(buffer.size()));
        if (len == 0) throw std::runtime_error("GetModuleFileNameA failed");
    }
    buffer.resize(len);
    return fs::path(buffer);
}

static fs::path root_dir() {
    fs::path parent = exe_path().parent_path();
    if (lower(parent.filename().string()) == "bin") return parent.parent_path();
    return fs::current_path();
}

static fs::path path_in_root(const std::string & rel) {
    return root_dir() / fs::path(rel);
}

static std::string read_file(const fs::path & p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void write_file(const fs::path & p, const std::string & content) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write " + p.string());
    out << content;
}

static std::optional<int> read_pid(const fs::path & p) {
    std::string s = trim(read_file(p));
    if (s.empty()) return std::nullopt;
    try { return std::stoi(s); } catch (...) { return std::nullopt; }
}

static bool process_alive(int pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!h) return false;
    DWORD code = 0;
    bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

static void terminate_process_tree(DWORD pid) {
    if (!pid) return;

    std::vector<DWORD> children;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        if (Process32First(snapshot, &entry)) {
            do {
                if (entry.th32ParentProcessID == pid) children.push_back(entry.th32ProcessID);
            } while (Process32Next(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
    for (DWORD child : children) terminate_process_tree(child);

    HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!h) return;
    TerminateProcess(h, 0);
    WaitForSingleObject(h, 5000);
    CloseHandle(h);
}

static bool is_our_llama_server(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    std::array<char, 32768> image{};
    DWORD length = static_cast<DWORD>(image.size());
    bool ok = QueryFullProcessImageNameA(process, 0, image.data(), &length) != FALSE;
    CloseHandle(process);
    if (!ok) return false;
    std::error_code ec;
    fs::path expected = fs::weakly_canonical(path_in_root("bin/llama-server.exe"), ec);
    fs::path actual = fs::weakly_canonical(fs::path(std::string(image.data(), length)), ec);
    return lower(expected.string()) == lower(actual.string());
}

static void stop_orphaned_llama_servers() {
    std::vector<DWORD> owned;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (Process32First(snapshot, &entry)) {
        do {
            if (_stricmp(entry.szExeFile, "llama-server.exe") == 0 && is_our_llama_server(entry.th32ProcessID)) {
                owned.push_back(entry.th32ProcessID);
            }
        } while (Process32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    for (DWORD owned_pid : owned) terminate_process_tree(owned_pid);
}

static void stop_pid_file(const fs::path & p, const std::string & name) {
    auto pid = read_pid(p);
    if (!pid || !process_alive(*pid)) {
        std::cout << name << " is not running\n";
        if (name == "llama-server") stop_orphaned_llama_servers();
        std::error_code ec;
        fs::remove(p, ec);
        return;
    }
    terminate_process_tree(DWORD(*pid));
    if (name == "llama-server") stop_orphaned_llama_servers();
    std::error_code ec;
    fs::remove(p, ec);
    std::cout << "stopped " << name << " pid " << *pid << "\n";
}

static std::string json_escape(const std::string & s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += char(c);
                }
        }
    }
    return out;
}

static std::optional<std::string> parse_json_string_at(const std::string & s, size_t quote_pos, size_t * end_pos = nullptr) {
    if (quote_pos >= s.size() || s[quote_pos] != '"') return std::nullopt;
    std::string out;
    for (size_t i = quote_pos + 1; i < s.size(); ++i) {
        char c = s[i];
        if (c == '"') {
            if (end_pos) *end_pos = i + 1;
            return out;
        }
        if (c == '\\' && i + 1 < s.size()) {
            char e = s[++i];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!parse_hex4(s, i + 1, cp)) {
                        out += '?';
                        break;
                    }
                    i += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < s.size() && s[i + 1] == '\\' && s[i + 2] == 'u') {
                        uint32_t low = 0;
                        if (parse_hex4(s, i + 3, low) && low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                            i += 6;
                        }
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: out += e; break;
            }
        } else {
            out += c;
        }
    }
    return std::nullopt;
}

static size_t find_key_colon(const std::string & json, const std::string & key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return pos;
    pos = json.find(':', pos + needle.size());
    return pos;
}

static std::optional<std::string> json_string_value(const std::string & json, const std::string & key) {
    size_t colon = find_key_colon(json, key);
    if (colon == std::string::npos) return std::nullopt;
    size_t q = json.find('"', colon + 1);
    if (q == std::string::npos) return std::nullopt;
    return parse_json_string_at(json, q);
}

static std::optional<int> json_int_value(const std::string & json, const std::string & key) {
    size_t colon = find_key_colon(json, key);
    if (colon == std::string::npos) return std::nullopt;
    size_t p = colon + 1;
    while (p < json.size() && std::isspace(static_cast<unsigned char>(json[p]))) p++;
    size_t e = p;
    if (e < json.size() && json[e] == '-') e++;
    while (e < json.size() && std::isdigit(static_cast<unsigned char>(json[e]))) e++;
    if (e == p) return std::nullopt;
    try { return std::stoi(json.substr(p, e - p)); } catch (...) { return std::nullopt; }
}

static bool json_bool_value(const std::string & json, const std::string & key, bool fallback) {
    size_t colon = find_key_colon(json, key);
    if (colon == std::string::npos) return fallback;
    std::string tail = lower(trim(json.substr(colon + 1, 8)));
    if (tail.rfind("true", 0) == 0) return true;
    if (tail.rfind("false", 0) == 0) return false;
    return fallback;
}

static std::optional<std::string> json_value_slice(const std::string & json, const std::string & key) {
    size_t colon = find_key_colon(json, key);
    if (colon == std::string::npos) return std::nullopt;
    size_t p = colon + 1;
    while (p < json.size() && std::isspace(static_cast<unsigned char>(json[p]))) p++;
    if (p >= json.size()) return std::nullopt;
    char open = json[p];
    char close = 0;
    if (open == '[') close = ']';
    else if (open == '{') close = '}';
    else if (open == '"') {
        size_t end = 0;
        if (!parse_json_string_at(json, p, &end)) return std::nullopt;
        return json.substr(p, end - p);
    } else {
        size_t e = p;
        while (e < json.size() && json[e] != ',' && json[e] != '}' && json[e] != '\r' && json[e] != '\n') e++;
        return trim(json.substr(p, e - p));
    }
    int depth = 0;
    bool in_str = false;
    bool esc = false;
    for (size_t i = p; i < json.size(); ++i) {
        char c = json[i];
        if (in_str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') in_str = true;
        else if (c == open) depth++;
        else if (c == close) {
            depth--;
            if (depth == 0) return json.substr(p, i - p + 1);
        }
    }
    return std::nullopt;
}

static std::string last_content_value(const std::string & json) {
    std::string value;
    std::string needle = "\"content\"";
    size_t pos = 0;
    while ((pos = json.find(needle, pos)) != std::string::npos) {
        size_t colon = json.find(':', pos + needle.size());
        if (colon == std::string::npos) break;
        size_t q = json.find('"', colon + 1);
        if (q == std::string::npos) break;
        if (auto s = parse_json_string_at(json, q)) value = *s;
        pos = q + 1;
    }
    return value;
}

class WinsockInit {
public:
    WinsockInit() {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) throw std::runtime_error("WSAStartup failed");
    }
    ~WinsockInit() { WSACleanup(); }
};

static std::atomic<DWORD> g_child_pid{0};
static std::atomic<bool> g_stop_requested{false};
static std::atomic<unsigned long long> g_request_id{1};
static std::mutex g_log_mutex;

static BOOL WINAPI console_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        g_stop_requested = true;
        DWORD pid = g_child_pid.load();
        if (pid) {
            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
            if (h) {
                TerminateProcess(h, 0);
                CloseHandle(h);
            }
        }
        return TRUE;
    }
    return FALSE;
}

static void send_all(SOCKET s, const std::string & data) {
    const char * p = data.data();
    size_t left = data.size();
    while (left > 0) {
        int n = send(s, p, int(std::min<size_t>(left, 1 << 20)), 0);
        if (n <= 0) throw std::runtime_error("socket send failed");
        p += n;
        left -= size_t(n);
    }
}

static std::string log_preview(std::string s, size_t limit = 300) {
    for (char & c : s) {
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    }
    if (s.size() > limit) s = s.substr(0, limit) + "...";
    return s;
}

static std::string log_timestamp() {
    SYSTEMTIME st{};
    GetSystemTime(&st);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                  unsigned(st.wYear), unsigned(st.wMonth), unsigned(st.wDay),
                  unsigned(st.wHour), unsigned(st.wMinute), unsigned(st.wSecond),
                  unsigned(st.wMilliseconds));
    return buf;
}

static void append_log_line(const fs::path & file, const std::string & line) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::app);
    out << log_timestamp() << " pid=" << GetCurrentProcessId()
        << " tid=" << GetCurrentThreadId() << " " << line << "\n";
}

static void proxy_log(const std::string & line) {
    append_log_line(path_in_root("logs/proxy-detailed.log"), line);
}

static void proxy_json_log(const std::string & json_object) {
    append_log_line(path_in_root("logs/proxy-requests.jsonl"), json_object);
}

static HttpResponse http_request(const std::string & host, int port, const std::string & method, const std::string & target,
                                 const std::string & body = {}, const std::map<std::string, std::string> & headers = {},
                                 DWORD timeout_ms = 900000) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo * result = nullptr;
    std::string port_s = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_s.c_str(), &hints, &result) != 0 || !result) {
        throw std::runtime_error("cannot resolve " + host);
    }
    SOCKET sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (sock == INVALID_SOCKET) {
        freeaddrinfo(result);
        throw std::runtime_error("cannot create socket");
    }
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout_ms), sizeof(timeout_ms));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeout_ms), sizeof(timeout_ms));
    if (connect(sock, result->ai_addr, int(result->ai_addrlen)) != 0) {
        freeaddrinfo(result);
        closesocket(sock);
        throw std::runtime_error("cannot connect to " + host + ":" + port_s);
    }
    freeaddrinfo(result);

    std::ostringstream req;
    req << method << " " << target << " HTTP/1.1\r\n";
    req << "Host: " << host << ":" << port << "\r\n";
    req << "Connection: close\r\n";
    for (const auto & [k, v] : headers) req << k << ": " << v << "\r\n";
    if (!body.empty()) req << "Content-Length: " << body.size() << "\r\n";
    req << "\r\n";
    req << body;
    send_all(sock, req.str());

    std::string raw;
    char buf[32768];
    for (;;) {
        int n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) break;
        raw.append(buf, buf + n);
    }
    closesocket(sock);

    HttpResponse r;
    r.raw = raw;
    size_t h = raw.find("\r\n\r\n");
    if (h != std::string::npos) {
        r.headers = raw.substr(0, h + 4);
        r.body = raw.substr(h + 4);
    } else {
        r.body = raw;
    }
    if (lower(r.headers).find("transfer-encoding: chunked") != std::string::npos) {
        r.body = decode_chunked(r.body);
    }
    size_t sp = raw.find(' ');
    if (sp != std::string::npos && sp + 4 <= raw.size()) {
        try { r.status = std::stoi(raw.substr(sp + 1, 3)); } catch (...) {}
    }
    return r;
}

static std::string http_reply(int status, const std::string & content_type, const std::string & body) {
    std::string reason = status == 200 ? "OK" : status == 404 ? "Not Found" : "Error";
    std::ostringstream out;
    out << "HTTP/1.1 " << status << " " << reason << "\r\n";
    out << "Content-Type: " << content_type << "\r\n";
    out << "Content-Length: " << body.size() << "\r\n";
    out << "Connection: close\r\n\r\n";
    out << body;
    return out.str();
}

static bool is_draft_gguf(const fs::path & path) {
    std::string name = lower(path.filename().string());
    return name.rfind("mtp-", 0) == 0 || name.find("-draft") != std::string::npos;
}

static std::string sha256_file(const fs::path & file) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0, hash_size = 0, size = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    auto close_algorithm = [&]() { if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); };
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &size, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_size), sizeof(hash_size), &size, 0) < 0) {
        close_algorithm();
        return {};
    }

    std::vector<UCHAR> object(object_size), digest(hash_size), buffer(1024 * 1024);
    if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) < 0) {
        close_algorithm();
        return {};
    }
    std::ifstream input(file, std::ios::binary);
    while (input) {
        input.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        std::streamsize read = input.gcount();
        if (read > 0 && BCryptHashData(hash, buffer.data(), static_cast<ULONG>(read), 0) < 0) {
            BCryptDestroyHash(hash);
            close_algorithm();
            return {};
        }
    }
    bool ok = input.eof() && BCryptFinishHash(hash, digest.data(), hash_size, 0) >= 0;
    BCryptDestroyHash(hash);
    close_algorithm();
    if (!ok) return {};

    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (UCHAR byte : digest) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 0x0f]);
    }
    return result;
}

static std::optional<fs::path> qwen_mtp_draft() {
    fs::path draft = path_in_root("models/mtp-Qwen3.8-27B-Q4_0.gguf");
    constexpr uintmax_t expected_size = 1680271648;
    constexpr const char * expected_sha256 = "051a1764cff8c4f3ee6ae8b00593a0364c7539c67fa50ffc58f3f96509fca38e";
    std::error_code ec;
    fs::path progress = draft.string() + ".aria2";
    if (fs::is_regular_file(draft, ec) && !fs::exists(progress, ec) && fs::file_size(draft, ec) == expected_size &&
        sha256_file(draft) == expected_sha256) return draft;
    return std::nullopt;
}

static std::vector<fs::path> gguf_files() {
    std::vector<fs::path> out;
    fs::path dir = path_in_root("models");
    if (!fs::exists(dir)) return out;
    for (const auto & e : fs::recursive_directory_iterator(dir)) {
        if (e.is_regular_file() && lower(e.path().extension().string()) == ".gguf" && !is_draft_gguf(e.path())) out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

static std::string default_model() {
    std::string state = trim(read_file(path_in_root("state/default-model.txt")));
    if (!state.empty()) return state;
    auto files = gguf_files();
    if (!files.empty()) return files.front().stem().string();
    return {};
}

static void write_default_model(const std::string & model) {
    write_file(path_in_root("state/default-model.txt"), model + "\n");
}

static std::string utc_timestamp() {
    SYSTEMTIME st{};
    GetSystemTime(&st);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02uZ",
                  unsigned(st.wYear), unsigned(st.wMonth), unsigned(st.wDay),
                  unsigned(st.wHour), unsigned(st.wMinute), unsigned(st.wSecond));
    return buf;
}

static std::string preferred_device() {
    if (fs::exists(path_in_root("bin/ggml-cuda.dll"))) return "CUDA0";
    if (fs::exists(path_in_root("bin/ggml-vulkan.dll"))) return "Vulkan0";
    return "";
}

static std::string backend_name() {
    if (fs::exists(path_in_root("bin/ggml-cuda.dll"))) return "CUDA";
    if (fs::exists(path_in_root("bin/ggml-vulkan.dll"))) return "Vulkan";
    return "CPU";
}

static std::string ollama_tags_json() {
    auto files = gguf_files();
    std::ostringstream out;
    out << "{\"models\":[";
    for (size_t i = 0; i < files.size(); ++i) {
        if (i) out << ",";
        std::string id = files[i].stem().string();
        uintmax_t size = 0;
        std::error_code ec;
        size = fs::file_size(files[i], ec);
        out << "{\"name\":\"" << json_escape(id) << "\",";
        out << "\"model\":\"" << json_escape(id) << "\",";
        out << "\"modified_at\":\"" << utc_timestamp() << "\",";
        out << "\"size\":" << size << ",";
        out << "\"digest\":\"\",";
        out << "\"details\":{\"format\":\"gguf\",\"family\":\"unknown\",\"families\":[],\"parameter_size\":\"\",\"quantization_level\":\"\"}}";
    }
    out << "]}";
    return out.str();
}

static void write_preset(int ctx, const Settings & s) {
    std::ostringstream out;
    out << "version = 1\n\n";
    out << "[*]\n";
    out << "c = " << ctx << "\n";
    if (s.gpu_layers >= 0) out << "n-gpu-layers = " << s.gpu_layers << "\n";
    out << "fit = " << (s.fit ? "on" : "off") << "\n";
    out << "fit-target = " << s.fit_target_mib << "\n";
    out << "flash-attn = on\n";
    if (s.no_host) out << "no-host = true\n";
    out << "kv-offload = " << (s.kv_offload ? "true" : "false") << "\n";
    out << "cache-type-k = " << s.cache_type_k << "\n";
    out << "cache-type-v = " << s.cache_type_v << "\n";
    out << "reasoning = off\n";
    out << "threads = " << s.threads << "\n";
    out << "threads-batch = " << s.threads_batch << "\n";
    out << "parallel = " << s.parallel << "\n";
    out << "jinja = true\n";
    out << "batch-size = " << s.batch_size << "\n";
    out << "ubatch-size = " << s.ubatch_size << "\n";

    if (s.profile == "fast") {
        auto draft = qwen_mtp_draft();
        if (draft) {
            for (const auto & model : gguf_files()) {
                if (lower(model.filename().string()).find("lowgpu") == std::string::npos) continue;
                out << "\n[" << model.stem().string() << "]\n";
                out << "model-draft = " << draft->string() << "\n";
                out << "spec-type = draft-mtp\n";
                out << "spec-draft-ngl = 999\n";
                out << "spec-draft-device = " << preferred_device() << "\n";
                out << "spec-draft-n-max = 2\n";
                out << "spec-draft-type-k = q4_0\n";
                out << "spec-draft-type-v = q4_0\n";
                out << "spec-draft-threads = 2\n";
                out << "spec-draft-threads-batch = 8\n";
            }
        }
    }
    write_file(path_in_root("config/models.preset.ini"), out.str());
}

static DWORD start_process(const std::string & cmdline, const fs::path & workdir, const fs::path & out_log, const fs::path & err_log) {
    fs::create_directories(out_log.parent_path());
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE out = CreateFileA(out_log.string().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE err = CreateFileA(err_log.string().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE || err == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open log files");
    HANDLE stdin_read = nullptr;
    HANDLE stdin_write = nullptr;
    if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0)) {
        CloseHandle(out);
        CloseHandle(err);
        throw std::runtime_error("cannot create stdin pipe");
    }
    SetFilePointer(out, 0, nullptr, FILE_END);
    SetFilePointer(err, 0, nullptr, FILE_END);

    STARTUPINFOA si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdin_read;
    si.hStdOutput = out;
    si.hStdError = err;

    std::string mutable_cmd = cmdline;
    DWORD flags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS | CREATE_BREAKAWAY_FROM_JOB;
    BOOL ok = CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE, flags, nullptr, workdir.string().c_str(), &si, &pi);
    if (!ok) {
        flags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS;
        mutable_cmd = cmdline;
        ok = CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE, flags, nullptr, workdir.string().c_str(), &si, &pi);
    }
    CloseHandle(stdin_read);
    CloseHandle(stdin_write);
    CloseHandle(out);
    CloseHandle(err);
    if (!ok) throw std::runtime_error("CreateProcess failed: " + cmdline);
    DWORD pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return pid;
}

static PROCESS_INFORMATION start_process_console(const std::string & cmdline, const fs::path & workdir) {
    STARTUPINFOA si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    std::string mutable_cmd = cmdline;
    BOOL ok = CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, workdir.string().c_str(), &si, &pi);
    if (!ok) throw std::runtime_error("CreateProcess failed: " + cmdline);
    return pi;
}

struct FirewallRule {
    const char * name;
    int port;
};

static const FirewallRule FIREWALL_RULES[] = {
    {"llamacpp server 8080 private subnet", 8080},
    {"llamacpp ollama proxy 11434 private subnet", 11434},
};

static bool is_process_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

static int shell_status_quiet(const std::string & cmd) {
    std::string quiet = cmd + " >nul 2>nul";
    return std::system(quiet.c_str());
}

static bool firewall_rule_exists(const FirewallRule & rule) {
    std::string cmd = "netsh advfirewall firewall show rule name=\"" + std::string(rule.name) + "\"";
    return shell_status_quiet(cmd) == 0;
}

static int add_firewall_rule(const FirewallRule & rule) {
    if (firewall_rule_exists(rule)) {
        std::cout << "present: " << rule.name << "\n";
        return 0;
    }
    std::ostringstream cmd;
    cmd << "netsh advfirewall firewall add rule name=\"" << rule.name
        << "\" dir=in action=allow protocol=TCP localport=" << rule.port
        << " profile=private remoteip=localsubnet";
    int rc = std::system(cmd.str().c_str());
    if (rc != 0 || !firewall_rule_exists(rule)) {
        std::cerr << "failed: " << rule.name << "\n";
        return 1;
    }
    std::cout << "added: " << rule.name << "\n";
    return 0;
}

static int request_firewall_elevation() {
    std::string args = "firewall --elevated";
    HINSTANCE result = ShellExecuteA(nullptr, "runas", exe_path().string().c_str(), args.c_str(), root_dir().string().c_str(), SW_SHOWNORMAL);
    INT_PTR code = reinterpret_cast<INT_PTR>(result);
    if (code <= 32) throw std::runtime_error("UAC elevation failed for firewall setup, ShellExecute code " + std::to_string(code));
    std::cout << "UAC elevation requested for firewall setup. Confirm the Windows prompt.\n";
    return 0;
}

static bool wait_health(int seconds) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        try {
            auto r = http_request("127.0.0.1", 8080, "GET", "/health");
            if (r.status >= 200 && r.status < 500) return true;
        } catch (...) {}
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return false;
}

static bool preload_model(const std::string & model) {
    if (model.empty()) return true;
    std::string body = "{\"model\":\"" + json_escape(model) + "\",\"messages\":[{\"role\":\"user\",\"content\":\"ping\"}],\"max_tokens\":1,\"stream\":false}";
    try {
        auto r = http_request("127.0.0.1", 8080, "POST", "/v1/chat/completions", body, {{"Content-Type", "application/json"}});
        return r.status >= 200 && r.status < 300;
    } catch (const std::exception & e) {
        std::cerr << "preload failed: " << e.what() << "\n";
        return false;
    }
}

static std::string build_llama_server_command(int ctx, const Settings & s) {
    (void)ctx;
    fs::path llama = path_in_root("bin/llama-server.exe");
    std::ostringstream cmd;
    cmd << quote_arg(llama.string())
        << " --host " << s.llama_host
        << " --port " << s.llama_port
        << " --models-dir " << quote_arg(path_in_root("models").string())
        << " --models-preset " << quote_arg(path_in_root("config/models.preset.ini").string())
        << " --prio 2"
        << " --poll 75"
        << " --reasoning off"
        << " --no-mmap";
    std::string dev = preferred_device();
    if (!dev.empty()) cmd << " --device " << dev;
    return cmd.str();
}

static std::string openai_content_from_response(const std::string & body) {
    size_t message = body.find("\"message\"");
    size_t pos = message == std::string::npos ? 0 : message;
    size_t content_key = body.find("\"content\"", pos);
    if (content_key == std::string::npos) content_key = body.find("\"text\"");
    if (content_key == std::string::npos) return {};
    size_t colon = body.find(':', content_key);
    if (colon == std::string::npos) return {};
    size_t q = body.find('"', colon + 1);
    if (q == std::string::npos) return {};
    std::string content = parse_json_string_at(body, q).value_or("");
    if (!content.empty()) return content;
    size_t reasoning_key = body.find("\"reasoning_content\"", pos);
    if (reasoning_key == std::string::npos) return content;
    colon = body.find(':', reasoning_key);
    if (colon == std::string::npos) return content;
    q = body.find('"', colon + 1);
    if (q == std::string::npos) return content;
    return parse_json_string_at(body, q).value_or(content);
}

static int openai_token_count(const std::string & body, const std::string & key) {
    return json_int_value(body, key).value_or(0);
}

static std::string openai_finish_reason(const std::string & body) {
    return json_string_value(body, "finish_reason").value_or("");
}

struct ResolvedTokenLimit {
    int value = 8192;
    std::string source = "proxy_default_8192";
};

static ResolvedTokenLimit normalize_token_limit(std::optional<int> raw, const std::string & source) {
    if (!raw) return {};
    if (*raw <= 0) return {8192, source + "_nonpositive_fallback_8192"};
    return {*raw, source};
}

static ResolvedTokenLimit resolve_ollama_max_tokens(const std::string & request_body) {
    if (auto options = json_value_slice(request_body, "options")) {
        if (auto n = json_int_value(*options, "num_predict")) return normalize_token_limit(n, "options.num_predict");
        if (auto n = json_int_value(*options, "max_tokens")) return normalize_token_limit(n, "options.max_tokens");
    }
    if (auto n = json_int_value(request_body, "num_predict")) return normalize_token_limit(n, "num_predict");
    if (auto n = json_int_value(request_body, "max_tokens")) return normalize_token_limit(n, "max_tokens");
    return {};
}

static std::string make_openai_body_from_ollama(const std::string & request_body, ResolvedTokenLimit * resolved_limit = nullptr) {
    std::string model = json_string_value(request_body, "model").value_or(default_model());
    if (model.empty()) model = "default";
    ResolvedTokenLimit limit = resolve_ollama_max_tokens(request_body);
    if (resolved_limit) *resolved_limit = limit;
    std::string messages;
    if (auto m = json_value_slice(request_body, "messages")) {
        messages = *m;
    } else {
        std::string prompt = json_string_value(request_body, "prompt").value_or(last_content_value(request_body));
        messages = "[{\"role\":\"user\",\"content\":\"" + json_escape(prompt) + "\"}]";
    }
    std::ostringstream out;
    out << "{\"model\":\"" << json_escape(model) << "\",";
    out << "\"messages\":" << messages << ",";
    out << "\"max_tokens\":" << limit.value << ",";
    out << "\"stream\":false";
    if (auto temp = json_value_slice(request_body, "temperature")) out << ",\"temperature\":" << *temp;
    if (auto top_p = json_value_slice(request_body, "top_p")) out << ",\"top_p\":" << *top_p;
    if (auto stop = json_value_slice(request_body, "stop")) out << ",\"stop\":" << *stop;
    out << "}";
    return out.str();
}

struct ParsedRequest {
    std::string method;
    std::string target;
    std::string path;
    std::map<std::string, std::string> headers;
    std::string body;
};

static ParsedRequest read_http_request(SOCKET client) {
    std::string data;
    char buf[8192];
    size_t header_end = std::string::npos;
    while ((header_end = data.find("\r\n\r\n")) == std::string::npos) {
        int n = recv(client, buf, sizeof(buf), 0);
        if (n <= 0) throw std::runtime_error("client disconnected");
        data.append(buf, buf + n);
        if (data.size() > 1024 * 1024) throw std::runtime_error("request headers too large");
    }
    std::string header_block = data.substr(0, header_end);
    std::istringstream hs(header_block);
    ParsedRequest req;
    std::string version;
    hs >> req.method >> req.target >> version;
    std::string line;
    std::getline(hs, line);
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t c = line.find(':');
        if (c == std::string::npos) continue;
        req.headers[lower(trim(line.substr(0, c)))] = trim(line.substr(c + 1));
    }
    req.path = req.target.substr(0, req.target.find('?'));
    size_t content_length = 0;
    if (auto it = req.headers.find("content-length"); it != req.headers.end()) {
        content_length = size_t(std::stoull(it->second));
    }
    req.body = data.substr(header_end + 4);
    while (req.body.size() < content_length) {
        int n = recv(client, buf, sizeof(buf), 0);
        if (n <= 0) throw std::runtime_error("client disconnected while reading body");
        req.body.append(buf, buf + n);
    }
    if (req.body.size() > content_length) req.body.resize(content_length);
    return req;
}

static std::string build_forward_request(const ParsedRequest & req) {
    std::ostringstream out;
    out << req.method << " " << req.target << " HTTP/1.1\r\n";
    out << "Host: 127.0.0.1:8080\r\n";
    out << "Connection: close\r\n";
    for (const auto & [k, v] : req.headers) {
        if (k == "host" || k == "connection" || k == "content-length") continue;
        out << k << ": " << v << "\r\n";
    }
    if (!req.body.empty()) out << "Content-Length: " << req.body.size() << "\r\n";
    out << "\r\n" << req.body;
    return out.str();
}

static std::string forward_raw_to_llama(const ParsedRequest & req) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo * result = nullptr;
    if (getaddrinfo("127.0.0.1", "8080", &hints, &result) != 0 || !result) throw std::runtime_error("cannot resolve llama-server");
    SOCKET s = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(result);
        throw std::runtime_error("cannot create upstream socket");
    }
    if (connect(s, result->ai_addr, int(result->ai_addrlen)) != 0) {
        freeaddrinfo(result);
        closesocket(s);
        throw std::runtime_error("cannot connect to llama-server");
    }
    freeaddrinfo(result);
    send_all(s, build_forward_request(req));
    std::string raw;
    char buf[32768];
    for (;;) {
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        raw.append(buf, buf + n);
    }
    closesocket(s);
    return raw;
}

static int raw_http_status(const std::string & raw) {
    size_t sp = raw.find(' ');
    if (sp == std::string::npos || sp + 4 > raw.size()) return 0;
    try { return std::stoi(raw.substr(sp + 1, 3)); } catch (...) { return 0; }
}

static std::string raw_http_body(const std::string & raw) {
    size_t h = raw.find("\r\n\r\n");
    if (h == std::string::npos) return {};
    std::string body = raw.substr(h + 4);
    std::string headers = lower(raw.substr(0, h + 4));
    if (headers.find("transfer-encoding: chunked") != std::string::npos) {
        body = decode_chunked(body);
    }
    return body;
}

static long long elapsed_ms(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}

static std::string handle_proxy_request(const ParsedRequest & req, unsigned long long request_id) {
    if (req.path.rfind("/v1/", 0) == 0) {
        auto start = std::chrono::steady_clock::now();
        std::string raw = forward_raw_to_llama(req);
        int status = raw_http_status(raw);
        std::string body = raw_http_body(raw);
        int prompt_tokens = openai_token_count(body, "prompt_tokens");
        int completion_tokens = openai_token_count(body, "completion_tokens");
        std::string finish = openai_finish_reason(body);
        long long ms = elapsed_ms(start);
        proxy_log("id=" + std::to_string(request_id) + " forward_v1 status=" + std::to_string(status) +
                  " path=" + req.path + " ms=" + std::to_string(ms) +
                  " request_bytes=" + std::to_string(req.body.size()) +
                  " response_bytes=" + std::to_string(body.size()) +
                  " prompt_tokens=" + std::to_string(prompt_tokens) +
                  " completion_tokens=" + std::to_string(completion_tokens) +
                  " finish_reason=" + finish);
        proxy_json_log("{\"id\":" + std::to_string(request_id) +
                       ",\"event\":\"forward_v1\",\"path\":\"" + json_escape(req.path) +
                       "\",\"status\":" + std::to_string(status) +
                       ",\"ms\":" + std::to_string(ms) +
                       ",\"request_bytes\":" + std::to_string(req.body.size()) +
                       ",\"response_bytes\":" + std::to_string(body.size()) +
                       ",\"prompt_tokens\":" + std::to_string(prompt_tokens) +
                       ",\"completion_tokens\":" + std::to_string(completion_tokens) +
                       ",\"finish_reason\":\"" + json_escape(finish) + "\"}");
        return raw;
    }
    if (req.path == "/api/version" && req.method == "GET") {
        proxy_log("id=" + std::to_string(request_id) + " api_version status=200");
        return http_reply(200, "application/json; charset=utf-8", "{\"version\":\"llama.cpp-cpp-proxy\"}");
    }
    if (req.path == "/api/tags" && req.method == "GET") {
        proxy_log("id=" + std::to_string(request_id) + " api_tags status=200");
        return http_reply(200, "application/json; charset=utf-8", ollama_tags_json());
    }
    if (req.path == "/api/ps" && req.method == "GET") {
        proxy_log("id=" + std::to_string(request_id) + " api_ps status=200");
        return http_reply(200, "application/json; charset=utf-8", ollama_tags_json());
    }
    if (req.path == "/api/show" && req.method == "POST") {
        std::string model = json_string_value(req.body, "model").value_or(default_model());
        std::string body = "{\"license\":\"\",\"modelfile\":\"\",\"parameters\":\"\",\"template\":\"\",\"details\":{\"format\":\"gguf\",\"family\":\"unknown\",\"families\":[],\"parameter_size\":\"\",\"quantization_level\":\"\"},\"model_info\":{\"name\":\"" + json_escape(model) + "\"}}";
        proxy_log("id=" + std::to_string(request_id) + " api_show status=200 model=" + model);
        return http_reply(200, "application/json; charset=utf-8", body);
    }
    if ((req.path == "/api/chat" || req.path == "/api/generate") && req.method == "POST") {
        auto start = std::chrono::steady_clock::now();
        std::string model = json_string_value(req.body, "model").value_or(default_model());
        if (model.empty()) model = "default";
        ResolvedTokenLimit limit;
        std::string openai_body = make_openai_body_from_ollama(req.body, &limit);
        bool requested_stream = json_bool_value(req.body, "stream", true);
        proxy_log("id=" + std::to_string(request_id) + " ollama_in path=" + req.path +
                  " model=" + model +
                  " stream=" + std::string(requested_stream ? "true" : "false") +
                  " max_tokens=" + std::to_string(limit.value) +
                  " max_tokens_source=" + limit.source +
                  " request_bytes=" + std::to_string(req.body.size()) +
                  " openai_bytes=" + std::to_string(openai_body.size()) +
                  " preview=\"" + json_escape(log_preview(req.body)) + "\"");
        auto upstream = http_request("127.0.0.1", 8080, "POST", "/v1/chat/completions", openai_body, {{"Content-Type", "application/json"}});
        if (upstream.status < 200 || upstream.status >= 300) {
            proxy_log("id=" + std::to_string(request_id) + " ollama_upstream_error status=" + std::to_string(upstream.status) +
                      " ms=" + std::to_string(elapsed_ms(start)) +
                      " body=\"" + json_escape(log_preview(upstream.body)) + "\"");
            return http_reply(500, "application/json; charset=utf-8", "{\"error\":\"llama-server request failed\"}");
        }
        std::string content = openai_content_from_response(upstream.body);
        std::string now = utc_timestamp();
        int prompt_tokens = openai_token_count(upstream.body, "prompt_tokens");
        int completion_tokens = openai_token_count(upstream.body, "completion_tokens");
        std::string finish = openai_finish_reason(upstream.body);
        long long ms = elapsed_ms(start);
        std::ostringstream obj;
        obj << "{\"model\":\"" << json_escape(model) << "\",\"created_at\":\"" << now << "\",";
        if (req.path == "/api/generate") obj << "\"response\":\"" << json_escape(content) << "\",";
        else obj << "\"message\":{\"role\":\"assistant\",\"content\":\"" << json_escape(content) << "\"},";
        obj << "\"done\":true";
        if (prompt_tokens > 0) obj << ",\"prompt_eval_count\":" << prompt_tokens;
        if (completion_tokens > 0) obj << ",\"eval_count\":" << completion_tokens;
        obj << "}";
        proxy_log("id=" + std::to_string(request_id) + " ollama_out status=200 path=" + req.path +
                  " model=" + model +
                  " ms=" + std::to_string(ms) +
                  " max_tokens=" + std::to_string(limit.value) +
                  " max_tokens_source=" + limit.source +
                  " finish_reason=" + finish +
                  " prompt_tokens=" + std::to_string(prompt_tokens) +
                  " completion_tokens=" + std::to_string(completion_tokens) +
                  " content_chars=" + std::to_string(content.size()) +
                  " upstream_bytes=" + std::to_string(upstream.body.size()));
        proxy_json_log("{\"id\":" + std::to_string(request_id) +
                       ",\"event\":\"ollama_complete\",\"path\":\"" + json_escape(req.path) +
                       "\",\"model\":\"" + json_escape(model) +
                       "\",\"status\":200,\"ms\":" + std::to_string(ms) +
                       ",\"max_tokens\":" + std::to_string(limit.value) +
                       ",\"max_tokens_source\":\"" + json_escape(limit.source) +
                       "\",\"finish_reason\":\"" + json_escape(finish) +
                       "\",\"prompt_tokens\":" + std::to_string(prompt_tokens) +
                       ",\"completion_tokens\":" + std::to_string(completion_tokens) +
                       ",\"content_chars\":" + std::to_string(content.size()) +
                       ",\"request_bytes\":" + std::to_string(req.body.size()) +
                       ",\"upstream_bytes\":" + std::to_string(upstream.body.size()) + "}");
        if (requested_stream) return http_reply(200, "application/x-ndjson; charset=utf-8", obj.str() + "\n");
        return http_reply(200, "application/json; charset=utf-8", obj.str());
    }
    proxy_log("id=" + std::to_string(request_id) + " unsupported status=404 method=" + req.method + " path=" + req.path);
    return http_reply(404, "application/json; charset=utf-8", "{\"error\":\"unsupported endpoint\"}");
}

static int cmd_proxy() {
    WinsockInit wsa;
    SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET) throw std::runtime_error("cannot create listener socket");
    BOOL reuse = TRUE;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(11434);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(server, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        closesocket(server);
        throw std::runtime_error("cannot bind proxy on 0.0.0.0:11434");
    }
    if (listen(server, SOMAXCONN) != 0) {
        closesocket(server);
        throw std::runtime_error("cannot listen on proxy socket");
    }
    std::cout << "C++ Ollama/OpenAI proxy listening on 0.0.0.0:11434 -> 127.0.0.1:8080\n";
    for (;;) {
        SOCKET client = accept(server, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        std::thread([client]() {
            unsigned long long request_id = g_request_id.fetch_add(1);
            auto start = std::chrono::steady_clock::now();
            try {
                ParsedRequest req = read_http_request(client);
                proxy_log("id=" + std::to_string(request_id) + " recv method=" + req.method +
                          " target=" + req.target +
                          " path=" + req.path +
                          " body_bytes=" + std::to_string(req.body.size()));
                std::string reply = handle_proxy_request(req, request_id);
                proxy_log("id=" + std::to_string(request_id) + " sent bytes=" + std::to_string(reply.size()) +
                          " total_ms=" + std::to_string(elapsed_ms(start)));
                send_all(client, reply);
            } catch (const std::exception & e) {
                proxy_log("id=" + std::to_string(request_id) + " exception total_ms=" + std::to_string(elapsed_ms(start)) +
                          " error=\"" + json_escape(e.what()) + "\"");
                std::string body = "{\"error\":\"" + json_escape(e.what()) + "\"}";
                try { send_all(client, http_reply(500, "application/json; charset=utf-8", body)); } catch (...) {}
            }
            closesocket(client);
        }).detach();
    }
}

static int cmd_start(bool no_proxy, bool no_preload, const std::string & profile) {
    WinsockInit wsa;
    Settings s = settings_for_profile(profile);
    fs::create_directories(path_in_root("logs"));
    fs::create_directories(path_in_root("state"));
    fs::create_directories(path_in_root("models"));
    fs::path llama = path_in_root("bin/llama-server.exe");
    if (!fs::exists(llama)) throw std::runtime_error("missing " + llama.string());

    stop_pid_file(path_in_root("state/llama-server.pid"), "llama-server");
    if (!no_proxy) stop_pid_file(path_in_root("state/ollama-proxy.pid"), "ollama-proxy");

    auto models = gguf_files();
    if (models.empty()) {
        std::cerr << "warning: no GGUF model in " << path_in_root("models").string() << "\n";
        no_preload = true;
    }
    std::string model = default_model();

    for (int ctx : s.contexts) {
        write_preset(ctx, s);
        DWORD pid = start_process(build_llama_server_command(ctx, s), root_dir(), path_in_root("logs/llama-server.out.log"), path_in_root("logs/llama-server.err.log"));
        write_file(path_in_root("state/llama-server.pid"), std::to_string(pid) + "\n");
        std::cout << "started llama-server pid " << pid << " profile=" << s.profile << " ctx=" << ctx << "\n";
        if (!wait_health(90)) {
            std::cerr << "llama-server did not become healthy for ctx " << ctx << "\n";
            stop_pid_file(path_in_root("state/llama-server.pid"), "llama-server");
            continue;
        }
        if (!no_preload && !preload_model(model)) {
            std::cerr << "preload failed for " << model << ", trying smaller context\n";
            stop_pid_file(path_in_root("state/llama-server.pid"), "llama-server");
            continue;
        }
        if (!no_proxy) {
            fs::path self = exe_path();
            DWORD ppid = start_process(quote_arg(self.string()) + " proxy", root_dir(), path_in_root("logs/ollama-proxy.out.log"), path_in_root("logs/ollama-proxy.err.log"));
            write_file(path_in_root("state/ollama-proxy.pid"), std::to_string(ppid) + "\n");
            std::cout << "started C++ proxy pid " << ppid << "\n";
        }
        std::cout << "OpenAI: http://127.0.0.1:8080/v1\n";
        std::cout << "Ollama: http://127.0.0.1:11434/api\n";
        return 0;
    }
    throw std::runtime_error("unable to start llama-server with configured contexts");
}

static int cmd_bench(const std::string & prompt);

static int cmd_watch_mtp() {
    SetConsoleCtrlHandler(console_handler, TRUE);
    fs::create_directories(path_in_root("state"));
    fs::path pid_file = path_in_root("state/mtp-watch.pid");
    write_file(pid_file, std::to_string(GetCurrentProcessId()) + "\n");
    std::cout << "waiting for verified Qwen MTP draft...\n";
    while (!g_stop_requested && !qwen_mtp_draft()) {
        std::this_thread::sleep_for(std::chrono::seconds(15));
    }
    std::error_code ec;
    fs::remove(pid_file, ec);
    if (g_stop_requested) return 0;

    std::cout << "verified Qwen MTP draft; restarting the fast server\n";
    int rc = cmd_start(false, false, "fast");
    if (rc != 0) return rc;
    return cmd_bench("Schreibe mindestens 450 zusammenhaengende deutsche Woerter ueber nachhaltige Staedte. Kein Titel und keine Liste.");
}

static std::string one_line(std::string s, size_t limit = 500) {
    for (char & c : s) {
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    }
    if (s.size() > limit) s = s.substr(0, limit) + "...";
    return s;
}

static bool expect_http(const std::string & label, const HttpResponse & r, int min_status = 200, int max_status = 299) {
    bool ok = r.status >= min_status && r.status <= max_status;
    std::cout << "[TEST] " << label << " -> HTTP " << r.status << (ok ? " OK" : " FAIL") << "\n";
    if (!r.body.empty()) std::cout << "       " << one_line(r.body) << "\n";
    return ok;
}

static bool run_api_tests() {
    bool ok = true;
    try {
        ok = expect_http("llama health", http_request("127.0.0.1", 8080, "GET", "/health")) && ok;
        ok = expect_http("OpenAI GET /v1/models", http_request("127.0.0.1", 8080, "GET", "/v1/models")) && ok;

        std::string model = default_model();
        std::string openai_body =
            "{\"model\":\"" + json_escape(model) +
            "\",\"messages\":[{\"role\":\"system\",\"content\":\"Antwort kurz.\"},{\"role\":\"user\",\"content\":\"Antworte exakt mit OK.\"}],\"max_tokens\":8,\"stream\":false}";
        ok = expect_http("OpenAI POST /v1/chat/completions",
                         http_request("127.0.0.1", 8080, "POST", "/v1/chat/completions", openai_body, {{"Content-Type", "application/json"}})) && ok;
        ok = expect_http("OpenAI via Ollama port POST /v1/chat/completions",
                         http_request("127.0.0.1", 11434, "POST", "/v1/chat/completions", openai_body, {{"Content-Type", "application/json"}})) && ok;

        ok = expect_http("Ollama GET /api/version", http_request("127.0.0.1", 11434, "GET", "/api/version")) && ok;
        ok = expect_http("Ollama GET /api/tags", http_request("127.0.0.1", 11434, "GET", "/api/tags")) && ok;

        std::string weird = "\\u00f6p\\u00fc\\u00fc\\u00fc\\u00b4+#.--..,\\\"\\u00a7$%&/=)(/=? -- antworte nur: EDGE_OK";
        std::string ollama_body =
            "{\"model\":\"" + json_escape(model) +
            "\",\"messages\":[{\"role\":\"user\",\"content\":\"" + weird +
            "\"}],\"options\":{\"num_predict\":16,\"temperature\":0},\"stream\":false}";
        ok = expect_http("Ollama POST /api/chat weird UTF-8 payload",
                         http_request("127.0.0.1", 11434, "POST", "/api/chat", ollama_body, {{"Content-Type", "application/json"}})) && ok;

        std::string generate_body =
            "{\"model\":\"" + json_escape(model) +
            "\",\"prompt\":\"Generate endpoint smoke test: antworte kurz.\",\"options\":{\"num_predict\":12,\"temperature\":0},\"stream\":false}";
        ok = expect_http("Ollama POST /api/generate",
                         http_request("127.0.0.1", 11434, "POST", "/api/generate", generate_body, {{"Content-Type", "application/json"}})) && ok;
    } catch (const std::exception & e) {
        std::cerr << "[TEST] exception: " << e.what() << "\n";
        ok = false;
    }
    return ok;
}

static void print_devices() {
    fs::path llama = path_in_root("bin/llama-server.exe");
    std::cout << "\n=== llama.cpp devices ===\n";
    std::string cmd = quote_arg(llama.string()) + " --list-devices";
    std::system(cmd.c_str());
    std::cout << "\n=== nvidia-smi ===\n";
    std::system("nvidia-smi --query-gpu=name,memory.used,memory.free,memory.total,utilization.gpu --format=csv,noheader");
}

static int cmd_run(bool exit_after_tests, const std::string & profile) {
    WinsockInit wsa;
    SetConsoleCtrlHandler(console_handler, TRUE);
    Settings s = settings_for_profile(profile);
    fs::create_directories(path_in_root("logs"));
    fs::create_directories(path_in_root("state"));
    fs::create_directories(path_in_root("models"));

    fs::path llama = path_in_root("bin/llama-server.exe");
    if (!fs::exists(llama)) throw std::runtime_error("missing " + llama.string());
    if (gguf_files().empty()) throw std::runtime_error("no GGUF model found in " + path_in_root("models").string());

    std::cout << "============================================================\n";
    std::cout << " llama.cpp RTX 5070 local server\n";
    std::cout << " Backend: " << backend_name() << "  Preferred device: " << (preferred_device().empty() ? "CPU" : preferred_device()) << "\n";
    std::cout << " Model:   " << default_model() << "\n";
    std::cout << " Profile: " << s.profile << "  KV: " << (s.kv_offload ? "GPU " + s.cache_type_k : "RAM " + s.cache_type_k) << "\n";
    std::cout << " RAM mode: --no-mmap (model is loaded into RAM), VRAM mode: "
              << (s.no_host ? "strict GPU buffers without host fallback" : "automatic fit + GPU offload") << "\n";
    std::cout << " APIs:    OpenAI http://127.0.0.1:8080/v1  |  Ollama http://127.0.0.1:11434/api\n";
    std::cout << "============================================================\n";
    print_devices();

    stop_pid_file(path_in_root("state/ollama-proxy.pid"), "ollama-proxy");
    stop_pid_file(path_in_root("state/llama-server.pid"), "llama-server");

    PROCESS_INFORMATION pi{};
    bool started = false;
    int used_ctx = 0;
    for (int ctx : s.contexts) {
        write_preset(ctx, s);
        std::cout << "\n[START] Trying context " << ctx << " with " << backend_name() << " / " << preferred_device() << "\n";
        pi = start_process_console(build_llama_server_command(ctx, s), root_dir());
        g_child_pid = pi.dwProcessId;
        write_file(path_in_root("state/llama-server.pid"), std::to_string(pi.dwProcessId) + "\n");

        if (!wait_health(180)) {
            std::cerr << "[START] Server did not become healthy at ctx " << ctx << "\n";
            terminate_process_tree(pi.dwProcessId);
            WaitForSingleObject(pi.hProcess, 15000);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            continue;
        }
        std::cout << "[START] Server is healthy. Loading model into RAM/VRAM...\n";
        if (!preload_model(default_model())) {
            std::cerr << "[START] Model preload failed at ctx " << ctx << ", trying lower context.\n";
            terminate_process_tree(pi.dwProcessId);
            WaitForSingleObject(pi.hProcess, 15000);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            continue;
        }
        used_ctx = ctx;
        started = true;
        break;
    }

    if (!started) throw std::runtime_error("unable to start and preload model with any configured context");

    std::thread proxy_thread([]() {
        try {
            cmd_proxy();
        } catch (const std::exception & e) {
            std::cerr << "[PROXY] " << e.what() << "\n";
        }
    });
    proxy_thread.detach();
    std::this_thread::sleep_for(std::chrono::seconds(2));

    std::cout << "\n[TEST] Running built-in API tests...\n";
    bool tests_ok = run_api_tests();
    std::cout << "\n=== GPU after load ===\n";
    std::system("nvidia-smi --query-gpu=name,memory.used,memory.free,memory.total,utilization.gpu --format=csv,noheader");

    std::cout << "\n============================================================\n";
    std::cout << " READY context=" << used_ctx << " tests=" << (tests_ok ? "OK" : "FAILED") << "\n";
    std::cout << " OpenAI base: http://127.0.0.1:8080/v1\n";
    std::cout << " Ollama API:  http://127.0.0.1:11434/api\n";
    std::cout << " LAN:         use this machine's LAN IP with ports 8080 or 11434\n";
    std::cout << " Stop:        Ctrl+C in this terminal\n";
    std::cout << "============================================================\n";

    if (exit_after_tests) {
        terminate_process_tree(pi.dwProcessId);
        WaitForSingleObject(pi.hProcess, 15000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        g_child_pid = 0;
        std::error_code ec;
        fs::remove(path_in_root("state/llama-server.pid"), ec);
        return tests_ok ? 0 : 2;
    }

    while (!g_stop_requested) {
        DWORD code = STILL_ACTIVE;
        if (!GetExitCodeProcess(pi.hProcess, &code) || code != STILL_ACTIVE) break;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    terminate_process_tree(pi.dwProcessId);
    WaitForSingleObject(pi.hProcess, 15000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    g_child_pid = 0;
    std::error_code ec;
    fs::remove(path_in_root("state/llama-server.pid"), ec);
    return tests_ok ? 0 : 2;
}

static int cmd_stop() {
    stop_pid_file(path_in_root("state/mtp-watch.pid"), "mtp-watch");
    stop_pid_file(path_in_root("state/ollama-proxy.pid"), "ollama-proxy");
    stop_pid_file(path_in_root("state/llama-server.pid"), "llama-server");
    return 0;
}

static int cmd_status() {
    WinsockInit wsa;
    auto lp = read_pid(path_in_root("state/llama-server.pid"));
    auto pp = read_pid(path_in_root("state/ollama-proxy.pid"));
    auto mp = read_pid(path_in_root("state/mtp-watch.pid"));
    std::cout << "root: " << root_dir().string() << "\n";
    std::cout << "llama-server pid: " << (lp ? std::to_string(*lp) : "-") << " alive=" << (lp && process_alive(*lp) ? "yes" : "no") << "\n";
    std::cout << "ollama-proxy pid: " << (pp ? std::to_string(*pp) : "-") << " alive=" << (pp && process_alive(*pp) ? "yes" : "no") << "\n";
    std::cout << "mtp-watch pid: " << (mp ? std::to_string(*mp) : "-") << " alive=" << (mp && process_alive(*mp) ? "yes" : "no") << "\n";
    std::cout << "default model: " << default_model() << "\n";
    std::cout << "models:\n";
    for (const auto & p : gguf_files()) std::cout << "  " << p.filename().string() << " (" << fs::file_size(p) << " bytes)\n";
    std::cout << "firewall rules:\n";
    for (const auto & rule : FIREWALL_RULES) {
        std::cout << "  " << rule.name << ": " << (firewall_rule_exists(rule) ? "present" : "missing") << "\n";
    }
    try {
        auto h = http_request("127.0.0.1", 8080, "GET", "/health");
        std::cout << "llama health: HTTP " << h.status << " " << trim(h.body) << "\n";
    } catch (const std::exception & e) {
        std::cout << "llama health: down (" << e.what() << ")\n";
    }
    try {
        auto v = http_request("127.0.0.1", 11434, "GET", "/api/version");
        std::cout << "proxy version: HTTP " << v.status << " " << trim(v.body) << "\n";
    } catch (const std::exception & e) {
        std::cout << "proxy version: down (" << e.what() << ")\n";
    }
    return 0;
}

static int cmd_firewall(bool elevated_child) {
    std::cout << "firewall target: Windows Private profile, LocalSubnet only, TCP ports 8080 and 11434\n";
    bool all_present = true;
    for (const auto & rule : FIREWALL_RULES) {
        if (!firewall_rule_exists(rule)) all_present = false;
    }
    if (all_present) {
        std::cout << "all firewall rules are already present\n";
        return 0;
    }
    if (!is_process_elevated()) {
        if (elevated_child) throw std::runtime_error("firewall setup still is not elevated");
        return request_firewall_elevation();
    }
    int rc = 0;
    for (const auto & rule : FIREWALL_RULES) rc |= add_firewall_rule(rule);
    return rc;
}

static int cmd_switch(const std::string & model, bool no_preload) {
    WinsockInit wsa;
    std::string selected = model;
    for (const auto & p : gguf_files()) {
        std::string stem = p.stem().string();
        if (stem == model || stem.find(model) != std::string::npos) {
            selected = stem;
            break;
        }
    }
    write_default_model(selected);
    std::cout << "default model set to " << selected << "\n";
    if (!no_preload && !preload_model(selected)) return 2;
    return 0;
}

struct CompareProfile {
    std::string name;
    int context = 65536;
    int fit_target_mib = 2048;
    bool gpu_kv = false;
    std::string cache_type = "q8_0";
    int threads = 8;
    int threads_batch = 8;
    bool strict_gpu = false;
    bool enable_mtp = true;
    int mtp_draft_n_max = 3;
    double mtp_draft_p_min = 0.0;
};

struct CompareResult {
    std::string case_id;
    std::string model;
    std::string profile;
    int context = 0;
    int http_status = 0;
    double elapsed_seconds = 0.0;
    int prompt_tokens = 0;
    int completion_tokens = 0;
    double completion_tokens_per_second = 0.0;
    double end_to_end_tokens_per_second = 0.0;
    int output_words = 0;
    std::string finish_reason;
    std::string status;
    std::string response;
    std::string error;
};

static int compare_fit_target(const fs::path & model) {
    return lower(model.filename().string()).find("lowgpu") != std::string::npos ? 512 : 2048;
}

static std::string compare_slug(std::string value) {
    for (char & c : value) {
        if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
    }
    return value;
}

static int count_words(const std::string & value) {
    int count = 0;
    bool in_word = false;
    for (unsigned char c : value) {
        bool word = !std::isspace(c);
        if (word && !in_word) ++count;
        in_word = word;
    }
    return count;
}

static void terminate_process(DWORD pid) {
    terminate_process_tree(pid);
}

static bool wait_health_on_port(int port, int seconds) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        try {
            auto r = http_request("127.0.0.1", port, "GET", "/health");
            if (r.status >= 200 && r.status < 500) return true;
        } catch (...) {}
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return false;
}

static std::string build_compare_server_command(const fs::path & model, int port, const CompareProfile & profile) {
    fs::path llama = path_in_root("bin/llama-server.exe");
    std::ostringstream cmd;
    cmd << quote_arg(llama.string())
        << " --model " << quote_arg(model.string())
        << " --alias " << quote_arg(model.stem().string())
        << " --host 127.0.0.1"
        << " --port " << port
        << " --ctx-size " << profile.context
        << " --flash-attn on"
        << " --cache-type-k " << profile.cache_type
        << " --cache-type-v " << profile.cache_type
        << " --threads " << profile.threads
        << " --threads-batch " << profile.threads_batch
        << " --parallel 1"
        << " --batch-size 2048"
        << " --ubatch-size 512"
        << " --prio 2"
        << " --poll 75"
        << " --no-mmap"
        << " --jinja"
        << " --reasoning off";
    if (profile.strict_gpu) {
        cmd << " --n-gpu-layers 999 --fit off --no-host --kv-offload";
    } else {
        cmd << " --fit on --fit-target " << profile.fit_target_mib;
        cmd << (profile.gpu_kv ? " --kv-offload" : " --no-kv-offload");
    }
    if (profile.enable_mtp && lower(model.filename().string()).find("lowgpu") != std::string::npos) {
        if (auto draft = qwen_mtp_draft()) {
            cmd << " --model-draft " << quote_arg(draft->string())
                << " --spec-type draft-mtp --n-gpu-layers-draft 999"
                << " --device-draft " << preferred_device()
                << " --spec-draft-n-max " << profile.mtp_draft_n_max
                << " --cache-type-k-draft q4_0 --cache-type-v-draft q4_0"
                << " --threads-draft 2 --threads-batch-draft 8";
            if (profile.mtp_draft_p_min > 0.0) {
                cmd << " --spec-draft-p-min " << profile.mtp_draft_p_min;
            }
        }
    }
    std::string dev = preferred_device();
    if (!dev.empty()) cmd << " --device " << dev;
    return cmd.str();
}

static void append_compare_result(const fs::path & file, const CompareResult & result) {
    std::ostringstream out;
    out << "{\"timestamp\":\"" << utc_timestamp() << "\""
        << ",\"case_id\":\"" << json_escape(result.case_id) << "\""
        << ",\"model\":\"" << json_escape(result.model) << "\""
        << ",\"profile\":\"" << json_escape(result.profile) << "\""
        << ",\"context\":" << result.context
        << ",\"status\":\"" << json_escape(result.status) << "\""
        << ",\"http_status\":" << result.http_status
        << ",\"elapsed_seconds\":" << result.elapsed_seconds
        << ",\"prompt_tokens\":" << result.prompt_tokens
        << ",\"completion_tokens\":" << result.completion_tokens
        << ",\"completion_tokens_per_second\":" << result.completion_tokens_per_second
        << ",\"end_to_end_tokens_per_second\":" << result.end_to_end_tokens_per_second
        << ",\"output_words\":" << result.output_words
        << ",\"finish_reason\":\"" << json_escape(result.finish_reason) << "\""
        << ",\"response\":\"" << json_escape(result.response) << "\""
        << ",\"error\":\"" << json_escape(result.error) << "\"}\n";
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::ofstream stream(file, std::ios::binary | std::ios::app);
    if (!stream) throw std::runtime_error("cannot append " + file.string());
    stream << out.str();
}

static bool compare_case_finished(const fs::path & file, const std::string & case_id) {
    std::string marker = "\"case_id\":\"" + json_escape(case_id) + "\",\"model\"";
    std::string content = read_file(file);
    size_t pos = 0;
    while ((pos = content.find(marker, pos)) != std::string::npos) {
        size_t line_end = content.find('\n', pos);
        std::string line = content.substr(pos, line_end == std::string::npos ? std::string::npos : line_end - pos);
        if (line.find("\"status\":\"ok\"") != std::string::npos) return true;
        pos += marker.size();
    }
    return false;
}

static CompareResult run_compare_request(int port, const fs::path & model, const CompareProfile & profile,
                                         const std::string & case_id, const std::string & prompt, int max_tokens) {
    CompareResult result;
    result.case_id = case_id;
    result.model = model.stem().string();
    result.profile = profile.name;
    result.context = profile.context;
    std::string body = "{\"model\":\"" + json_escape(result.model) + "\",\"messages\":[{\"role\":\"user\",\"content\":\"" + json_escape(prompt) + "\"}],\"temperature\":0,\"max_tokens\":" + std::to_string(max_tokens) + ",\"stream\":false}";
    auto started = std::chrono::steady_clock::now();
    try {
        DWORD timeout_ms = profile.context >= 65536 ? 43200000 : 7200000;
        auto response = http_request("127.0.0.1", port, "POST", "/v1/chat/completions", body, {{"Content-Type", "application/json"}}, timeout_ms);
        result.elapsed_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        result.http_status = response.status;
        result.response = openai_content_from_response(response.body);
        result.prompt_tokens = openai_token_count(response.body, "prompt_tokens");
        result.completion_tokens = openai_token_count(response.body, "completion_tokens");
        if (result.elapsed_seconds > 0.0) {
            result.completion_tokens_per_second = result.completion_tokens / result.elapsed_seconds;
            result.end_to_end_tokens_per_second = (result.prompt_tokens + result.completion_tokens) / result.elapsed_seconds;
        }
        result.output_words = count_words(result.response);
        result.finish_reason = openai_finish_reason(response.body);
        result.status = response.status >= 200 && response.status < 300 ? "ok" : "http_error";
        if (result.status != "ok") result.error = log_preview(response.body, 1000);
    } catch (const std::exception & e) {
        result.elapsed_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        result.status = "request_error";
        result.error = e.what();
    }
    return result;
}

static std::string synthetic_context_prompt(int context) {
    int repetitions = std::max(1, (context - 4096) / 2);
    std::string prompt;
    prompt.reserve(size_t(repetitions) * 11 + 256);
    prompt = "Lies den folgenden synthetischen Kontext. Antworte danach ausschliesslich mit CONTEXT_OK.\n\n";
    for (int i = 0; i < repetitions; ++i) prompt += " alpha beta";
    prompt += "\n\nPruefung: Antworte ausschliesslich mit CONTEXT_OK.";
    return prompt;
}

static bool model_matches(const fs::path & model, const std::string & fragment) {
    return fragment.empty() || lower(model.filename().string()).find(lower(fragment)) != std::string::npos;
}

static int run_model_server_cases(const fs::path & model, const CompareProfile & profile,
                                  const std::vector<std::pair<std::string, std::pair<std::string, int>>> & cases,
                                  const fs::path & result_file, bool resume, int port) {
    std::string stem = model.stem().string();
    bool has_work = false;
    for (const auto & item : cases) {
        if (!resume || !compare_case_finished(result_file, stem + "/" + profile.name + "/" + item.first)) {
            has_work = true;
            break;
        }
    }
    if (!has_work) {
        std::cout << "resume: " << stem << " " << profile.name << " already complete\n";
        return 0;
    }

    fs::path run_dir = result_file.parent_path() / compare_slug(stem + "_" + profile.name + "_c" + std::to_string(profile.context));
    DWORD pid = start_process(build_compare_server_command(model, port, profile), root_dir(), run_dir / "server.out.log", run_dir / "server.err.log");
    std::cout << "start " << stem << " profile=" << profile.name << " context=" << profile.context << " pid=" << pid << "\n";
    if (!wait_health_on_port(port, 600)) {
        CompareResult failed;
        failed.case_id = stem + "/" + profile.name + "/startup";
        failed.model = stem;
        failed.profile = profile.name;
        failed.context = profile.context;
        failed.status = "startup_error";
        failed.error = "health endpoint did not become ready within 600 seconds";
        append_compare_result(result_file, failed);
        terminate_process(pid);
        return 1;
    }

    int failures = 0;
    for (const auto & item : cases) {
        std::string case_id = stem + "/" + profile.name + "/" + item.first;
        if (resume && compare_case_finished(result_file, case_id)) continue;
        CompareResult result = run_compare_request(port, model, profile, case_id, item.second.first, item.second.second);
        append_compare_result(result_file, result);
        std::cout << "  " << item.first << " status=" << result.status << " http=" << result.http_status
                  << " elapsed=" << result.elapsed_seconds << "s output=" << result.completion_tokens << " tok"
                  << " rate=" << result.completion_tokens_per_second << " tok/s"
                  << " words=" << result.output_words << " finish=" << result.finish_reason << "\n";
        if (result.status != "ok") ++failures;
    }
    terminate_process(pid);
    return failures;
}

static int cmd_compare(const std::string & fragment, bool resume) {
    WinsockInit wsa;
    fs::path run_dir = path_in_root("logs/model-comparison");
    fs::create_directories(run_dir);
    fs::path result_file = run_dir / "results.jsonl";
    std::vector<std::pair<std::string, std::pair<std::string, int>>> cases = {
        {"speed_128", {"Gib das Wort alpha genau 128-mal aus, nur durch einzelne Leerzeichen getrennt. Gib keine anderen Zeichen aus.", 256}},
        {"reasoning_json", {"Ein Team hat 5 Aufgaben. Zwei dauern je 3 Stunden, drei dauern je 2 Stunden. Erzeuge ausschliesslich valides JSON mit den Schluesseln total_hours, tasks und explanation. Keine Markdown-Umrandung.", 512}},
        {"trees_1000_words", {"Schreibe einen zusammenhaengenden deutschen Sachtext mit genau 1000 Woertern ueber Baeume. Keine Ueberschrift, keine Listen, kein Nachwort. Beende den Text erst nach dem tausendsten Wort.", 2048}},
    };
    int failures = 0;
    int tested = 0;
    for (const auto & model : gguf_files()) {
        if (!model_matches(model, fragment)) continue;
        ++tested;
        if (lower(model.filename().string()).find("lowgpu") != std::string::npos) {
            CompareProfile fast_profile{"fast_gpu_kv_q4", 32768, 512, true, "q4_0", 2, 8, true};
            failures += run_model_server_cases(model, fast_profile, {cases.front()}, result_file, resume, 18080);
        }
        CompareProfile profile{"standard_host_kv_q8", 65536, compare_fit_target(model), false, "q8_0"};
        failures += run_model_server_cases(model, profile, cases, result_file, resume, 18080);
    }
    if (!tested) throw std::runtime_error("no model matches '" + fragment + "'");
    std::cout << "comparison results: " << result_file.string() << "\n";
    return failures ? 2 : 0;
}

static void start_mtp_watcher_if_needed() {
    if (qwen_mtp_draft()) return;
    fs::path pid_file = path_in_root("state/mtp-watch.pid");
    if (auto pid = read_pid(pid_file); pid && process_alive(*pid)) return;
    fs::path self = exe_path();
    DWORD pid = start_process(quote_arg(self.string()) + " watch-mtp", root_dir(),
                              path_in_root("logs/mtp-watch.out.log"), path_in_root("logs/mtp-watch.err.log"));
    write_file(pid_file, std::to_string(pid) + "\n");
    std::cout << "started MTP watcher pid " << pid << "\n";
}

static int cmd_mtp_tune(bool resume) {
    WinsockInit wsa;
    if (!qwen_mtp_draft()) throw std::runtime_error("verified Qwen MTP draft is required");

    std::optional<fs::path> model;
    for (const auto & candidate : gguf_files()) {
        if (lower(candidate.filename().string()).find("lowgpu") != std::string::npos) {
            model = candidate;
            break;
        }
    }
    if (!model) throw std::runtime_error("no LowGPU GGUF available for MTP tuning");

    fs::path run_dir = path_in_root("logs/mtp-tuning");
    fs::create_directories(run_dir);
    fs::path result_file = run_dir / "results.jsonl";
    const std::string natural_text_prompt =
        "Schreibe einen zusammenhaengenden deutschen Sachtext ueber Baeume. "
        "Schreibe fortlaufend weiter und beende den Text nicht vor dem Tokenlimit.";
    std::vector<CompareProfile> profiles = {
        {"mtp_n2", 8192, 512, true, "q4_0", 16, 16, false, true, 2, 0.0},
        {"mtp_n3", 8192, 512, true, "q4_0", 16, 16, false, true, 3, 0.0},
        {"mtp_n4", 8192, 512, true, "q4_0", 16, 16, false, true, 4, 0.0},
        {"mtp_n4_p080", 8192, 512, true, "q4_0", 16, 16, false, true, 4, 0.8},
    };

    cmd_stop();
    int failures = 0;
    for (const auto & profile : profiles) {
        failures += run_model_server_cases(*model, profile,
                                           {{"natural_text_512", {natural_text_prompt, 512}}},
                                           result_file, resume, 18080);
    }
    int restore = cmd_start(false, false, "fast");
    std::cout << "MTP tuning results: " << result_file.string() << "\n";
    return failures || restore ? 2 : 0;
}

static int cmd_tune(const std::string & fragment, bool resume) {
    WinsockInit wsa;
    fs::path run_dir = path_in_root("logs/performance-tuning");
    fs::create_directories(run_dir);
    fs::path result_file = run_dir / "results.jsonl";

    // Direct benchmark instances require exclusive VRAM; restore the public APIs when done.
    cmd_stop();

    const std::string speed_prompt =
        "Gib das Wort alpha genau 256-mal aus, nur durch einzelne Leerzeichen getrennt. Gib keine anderen Zeichen aus.";
    std::vector<CompareProfile> profiles = {
        {"auto_gpu_kv_q4_c8192_t8", 8192, 512, true, "q4_0", 8, 8},
        {"auto_gpu_kv_q4_c8192_t16", 8192, 512, true, "q4_0", 16, 16},
        {"auto_gpu_kv_q4_c8192_t32", 8192, 512, true, "q4_0", 32, 32},
        {"auto_gpu_kv_q4_c32768_t16", 32768, 512, true, "q4_0", 16, 16},
        {"auto_gpu_kv_q4_c65536_t16", 65536, 512, true, "q4_0", 16, 16},
    };

    int failures = 0;
    int tested = 0;
    for (const auto & model : gguf_files()) {
        if (!model_matches(model, fragment)) continue;
        ++tested;
        for (const auto & profile : profiles) {
            std::vector<std::pair<std::string, std::pair<std::string, int>>> cases = {
                {"speed_256", {speed_prompt, 256}},
            };
            failures += run_model_server_cases(model, profile, cases, result_file, resume, 18080);
        }
    }
    if (!tested) throw std::runtime_error("no model matches '" + fragment + "'");

    int restore = cmd_start(false, false, "fast");
    start_mtp_watcher_if_needed();
    std::cout << "tuning results: " << result_file.string() << "\n";
    return failures || restore ? 2 : 0;
}

static int cmd_context_probe(const std::string & fragment, bool resume) {
    WinsockInit wsa;
    fs::path run_dir = path_in_root("logs/context-probe");
    fs::create_directories(run_dir);
    fs::path result_file = run_dir / "results.jsonl";

    std::string selected = fragment.empty() ? default_model() : fragment;
    if (selected.empty()) throw std::runtime_error("contextprobe needs a default model or --model");

    std::optional<fs::path> model;
    for (const auto & candidate : gguf_files()) {
        if (model_matches(candidate, selected)) {
            model = candidate;
            break;
        }
    }
    if (!model) throw std::runtime_error("no model matches '" + selected + "'");

    // Probe an active prompt, not only a preallocated KV cache, at practical short and long contexts.
    cmd_stop();
    int failures = 0;
    for (int context : {8192, 32768}) {
        CompareProfile profile{"active_context_q4_c" + std::to_string(context) + "_t16",
                               context, 512, true, "q4_0", 16, 16};
        std::vector<std::pair<std::string, std::pair<std::string, int>>> cases = {
            {"active_context", {synthetic_context_prompt(context), 64}},
        };
        failures += run_model_server_cases(*model, profile, cases, result_file, resume, 18080);
    }

    int restore = cmd_start(false, false, "fast");
    std::cout << "context probe results: " << result_file.string() << "\n";
    return failures || restore ? 2 : 0;
}

static int cmd_longtest(const std::string & fragment, bool resume) {
    WinsockInit wsa;
    fs::path run_dir = path_in_root("logs/full-context");
    fs::create_directories(run_dir);
    fs::path result_file = run_dir / "results.jsonl";
    const std::vector<int> contexts = {65536, 131072, 262144};
    int failures = 0;
    int tested = 0;
    for (const auto & model : gguf_files()) {
        if (!model_matches(model, fragment)) continue;
        ++tested;
        for (int context : contexts) {
            CompareProfile profile{"full_context_host_kv_q8", context, compare_fit_target(model), false, "q8_0"};
            std::vector<std::pair<std::string, std::pair<std::string, int>>> cases = {
                {"depth_" + std::to_string(context - 4096), {synthetic_context_prompt(context), 64}},
            };
            failures += run_model_server_cases(model, profile, cases, result_file, resume, 18080);
        }
    }
    if (!tested) throw std::runtime_error("no model matches '" + fragment + "'");
    std::cout << "full-context results: " << result_file.string() << "\n";
    return failures ? 2 : 0;
}

static int cmd_bench(const std::string & prompt) {
    WinsockInit wsa;
    std::string model = default_model();
    if (model.empty()) throw std::runtime_error("no default model");
    std::string body = "{\"model\":\"" + json_escape(model) + "\",\"messages\":[{\"role\":\"user\",\"content\":\"" + json_escape(prompt) + "\"}],\"max_tokens\":512,\"stream\":false}";
    auto t0 = std::chrono::steady_clock::now();
    auto r = http_request("127.0.0.1", 8080, "POST", "/v1/chat/completions", body, {{"Content-Type", "application/json"}});
    auto t1 = std::chrono::steady_clock::now();
    std::chrono::duration<double> dt = t1 - t0;
    std::cout << openai_content_from_response(r.body) << "\n\n";
    int toks = openai_token_count(r.body, "completion_tokens");
    std::cout << "elapsed: " << dt.count() << "s\n";
    if (toks > 0) std::cout << "completion tokens: " << toks << "\ntokens/s: " << (toks / dt.count()) << "\n";
    return 0;
}

static int cmd_downloads() {
    fs::path d = path_in_root("downloads");
    if (!fs::exists(d)) {
        std::cout << "downloads directory does not exist\n";
        return 0;
    }
    for (const auto & e : fs::directory_iterator(d)) {
        std::cout << (e.is_directory() ? "[dir]  " : "[file] ") << e.path().filename().string();
        if (e.is_regular_file()) std::cout << "  " << fs::file_size(e.path()) << " bytes";
        std::cout << "\n";
    }
    return 0;
}

static void usage() {
    std::cout <<
        "lamacpp-local commands:\n"
        "  run [--fast|--balanced|--long] [--exit-after-tests]\n"
        "  start [--fast|--balanced|--long] [--no-proxy] [--no-preload]\n"
        "  stop\n"
        "  status\n"
        "  switch <model-fragment> [--no-preload]\n"
        "  bench [prompt]\n"
        "  compare [--model <fragment>] [--resume]\n"
        "  tune [--model <fragment>] [--resume]\n"
        "  mtptune [--resume]\n"
        "  contextprobe [--model <fragment>] [--resume]\n"
        "  longtest [--model <fragment>] [--resume]\n"
        "  watch-mtp\n"
        "  proxy\n"
        "  downloads\n"
        "  firewall\n";
}

int main(int argc, char ** argv) {
    SetConsoleOutputCP(CP_UTF8);
    try {
        if (argc < 2) {
            usage();
            return 1;
        }
        std::string cmd = lower(argv[1]);
        if (cmd == "proxy") return cmd_proxy();
        if (cmd == "run") {
            bool exit_after_tests = false;
            std::string profile = "fast";
            for (int i = 2; i < argc; ++i) {
                std::string a = lower(argv[i]);
                if (a == "--exit-after-tests") exit_after_tests = true;
                else if (a == "--fast") profile = "fast";
                else if (a == "--balanced") profile = "balanced";
                else if (a == "--long") profile = "long";
                else throw std::runtime_error("unknown run option: " + std::string(argv[i]));
            }
            return cmd_run(exit_after_tests, profile);
        }
        if (cmd == "start") {
            bool no_proxy = false, no_preload = false;
            std::string profile = "fast";
            for (int i = 2; i < argc; ++i) {
                std::string a = lower(argv[i]);
                if (a == "--no-proxy") no_proxy = true;
                else if (a == "--no-preload") no_preload = true;
                else if (a == "--fast") profile = "fast";
                else if (a == "--balanced") profile = "balanced";
                else if (a == "--long") profile = "long";
                else throw std::runtime_error("unknown start option: " + std::string(argv[i]));
            }
            return cmd_start(no_proxy, no_preload, profile);
        }
        if (cmd == "stop") return cmd_stop();
        if (cmd == "status") return cmd_status();
        if (cmd == "watch-mtp") return cmd_watch_mtp();
        if (cmd == "firewall") {
            bool elevated_child = false;
            for (int i = 2; i < argc; ++i) if (lower(argv[i]) == "--elevated") elevated_child = true;
            return cmd_firewall(elevated_child);
        }
        if (cmd == "switch") {
            if (argc < 3) throw std::runtime_error("switch needs a model name or fragment");
            bool no_preload = false;
            for (int i = 3; i < argc; ++i) if (lower(argv[i]) == "--no-preload") no_preload = true;
            return cmd_switch(argv[2], no_preload);
        }
        if (cmd == "bench") {
            std::string prompt = argc >= 3 ? argv[2] : "Schreibe eine kurze technische Zusammenfassung von llama.cpp.";
            return cmd_bench(prompt);
        }
        if (cmd == "compare" || cmd == "tune" || cmd == "contextprobe" || cmd == "longtest") {
            std::string fragment;
            bool resume = false;
            for (int i = 2; i < argc; ++i) {
                std::string a = lower(argv[i]);
                if (a == "--resume") {
                    resume = true;
                } else if (a == "--model") {
                    if (i + 1 >= argc) throw std::runtime_error("--model needs a fragment");
                    fragment = argv[++i];
                } else {
                    throw std::runtime_error("unknown " + cmd + " option: " + std::string(argv[i]));
                }
            }
            if (cmd == "compare") return cmd_compare(fragment, resume);
            if (cmd == "tune") return cmd_tune(fragment, resume);
            if (cmd == "contextprobe") return cmd_context_probe(fragment, resume);
            return cmd_longtest(fragment, resume);
        }
        if (cmd == "mtptune") {
            bool resume = false;
            for (int i = 2; i < argc; ++i) {
                if (lower(argv[i]) == "--resume") resume = true;
                else throw std::runtime_error("unknown mtptune option: " + std::string(argv[i]));
            }
            return cmd_mtp_tune(resume);
        }
        if (cmd == "downloads") return cmd_downloads();
        usage();
        return 1;
    } catch (const std::exception & e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
