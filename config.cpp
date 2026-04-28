#include "config.h"
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <sys/stat.h>

static std::string config_path() {
    const char* home = getenv("HOME");
    if (!home) home = ".";
    return std::string(home) + "/.config/thermoprint/config.json";
}

static std::string read_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void write_file(const std::string& path, const std::string& content) {
    std::string dir = path.substr(0, path.rfind('/'));
    if (!dir.empty())
        (void) system(("mkdir -p " + dir).c_str());
    std::ofstream out(path);
    out << content;
}

static std::string json_get(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.length();
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (json[pos] == '"') {
        ++pos;
        size_t end = json.find('"', pos);
        if (end == std::string::npos) return "";
        return json.substr(pos, end - pos);
    } else {
        size_t end = pos;
        while (end < json.length() && (isdigit(json[end]) || json[end] == '-' || json[end] == 't' || json[end] == 'f')) ++end;
        return json.substr(pos, end - pos);
    }
}

Config load_config() {
    Config cfg;
    std::string raw = read_file(config_path());
    if (raw.empty()) return cfg;

    std::string val;
    val = json_get(raw, "dither");     if (!val.empty()) cfg.dither = val;
    val = json_get(raw, "density");    if (!val.empty()) cfg.density = atoi(val.c_str());
    val = json_get(raw, "font_size");  if (!val.empty()) cfg.font_size = atoi(val.c_str());
    val = json_get(raw, "label_size_mm"); if (!val.empty()) cfg.label_size_mm = atoi(val.c_str());
    val = json_get(raw, "portrait");   if (val == "true") cfg.portrait = true;
    val = json_get(raw, "alignment");  if (!val.empty()) cfg.alignment = val;
    val = json_get(raw, "margin_left");   if (!val.empty()) cfg.margin_left   = atoi(val.c_str());
    val = json_get(raw, "margin_right");  if (!val.empty()) cfg.margin_right  = atoi(val.c_str());
    val = json_get(raw, "margin_top");    if (!val.empty()) cfg.margin_top    = atoi(val.c_str());
    val = json_get(raw, "margin_bottom"); if (!val.empty()) cfg.margin_bottom = atoi(val.c_str());
    val = json_get(raw, "last_mac");   if (!val.empty()) cfg.last_mac = val;

    if (cfg.font_size < 1) cfg.font_size = 1;
    if (cfg.font_size > 5) cfg.font_size = 5;
    if (cfg.margin_left   < 0) cfg.margin_left   = 0;
    if (cfg.margin_right  < 0) cfg.margin_right  = 0;
    if (cfg.margin_top    < 0) cfg.margin_top    = 0;
    if (cfg.margin_bottom < 0) cfg.margin_bottom = 0;
    if (cfg.alignment != "left" && cfg.alignment != "center" && cfg.alignment != "right")
        cfg.alignment = "left";
    return cfg;
}

void save_config(const Config& cfg) {
    std::ostringstream ss;
    ss << "{\n";
    ss << "    \"dither\": \"" << cfg.dither << "\",\n";
    ss << "    \"density\": " << cfg.density << ",\n";
    ss << "    \"font_size\": " << cfg.font_size << ",\n";
    ss << "    \"label_size_mm\": " << cfg.label_size_mm << ",\n";
    ss << "    \"portrait\": " << (cfg.portrait ? "true" : "false") << ",\n";
    ss << "    \"alignment\": \"" << cfg.alignment << "\",\n";
    ss << "    \"margin_left\": " << cfg.margin_left << ",\n";
    ss << "    \"margin_right\": " << cfg.margin_right << ",\n";
    ss << "    \"margin_top\": " << cfg.margin_top << ",\n";
    ss << "    \"margin_bottom\": " << cfg.margin_bottom << ",\n";
    ss << "    \"last_mac\": \"" << cfg.last_mac << "\"\n";
    ss << "}\n";
    write_file(config_path(), ss.str());
}