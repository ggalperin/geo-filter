/**
 * geo-filter
 *
 * HTTP service: "Is this IP in any of these allowed countries?"
 *
 * Routes:
 *   POST /check   – { "ip":"1.2.3.4", "allowed_countries":["US","GB","CA"] }
 *                   -> { "allowed":true, "country":"US", "ip":"1.2.3.4" }
 *   POST /reload  – hot-reload the MMDB from disk
 *
 * Environment variables:
 *   MMDB_PATH            Path to GeoLite2-Country.mmdb  (default /data/GeoLite2-Country.mmdb)
 *   LISTEN_HOST          Bind address                   (default 0.0.0.0)
 *   LISTEN_PORT          TCP port                       (default 8080)
 *   DB_RELOAD_INTERVAL   Seconds between watcher ticks  (default 3600)
 */

#include <iostream>
#include <string>
#include <unordered_set>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <filesystem>

#include "httplib.h"    // From https://github.com/yhirose/cpp-httplib/blob/master/httplib.h
#include "json.hpp"     // From https://github.com/nlohmann/json/blob/develop/include/nlohmann/json.hpp

#include <maxminddb.h>  // From https://github.com/maxmind/libmaxminddb/blob/main/include/maxminddb.h

using json = nlohmann::json;
namespace fs = std::filesystem;

// Config
struct Config {
    std::string mmdb_path              = "/data/GeoLite2-Country.mmdb";
    std::string host                   = "0.0.0.0";
    int         port                   = 8080;
    int         db_reload_interval_sec = 3600;

    Config() {
        auto env = [](const char* k, const char* d) -> std::string {
            const char* v = std::getenv(k); return v ? v : d;
        };
        mmdb_path              = env("MMDB_PATH", mmdb_path.c_str());
        host                   = env("LISTEN_HOST", host.c_str());
        try {
            port                   = std::stoi(env("LISTEN_PORT", "8080"));
            db_reload_interval_sec = std::stoi(env("DB_RELOAD_INTERVAL", "3600"));
        catch(const std::invalid_argument& e) {
            std::cerr << "Invalid argument. " << e.what():
        }
    }
};

// GeoDatabase 
class GeoDatabase {
public:
    explicit GeoDatabase(const std::string& path) : path_(path) {
        if (MMDB_open(path_.c_str(), MMDB_MODE_MMAP, &mmdb_) != MMDB_SUCCESS) {
            std::cerr << "[GeoDatabase] WARNING: cannot open " << path_
                      << " -- service will return 503 until DB is present\n";
        } else {
            open_      = true;
            loaded_at_ = std::chrono::system_clock::now();
            std::cout << "[GeoDatabase] opened: " << path_ << "\n";
        }
    }

    ~GeoDatabase() {
        std::unique_lock lk(mutex_);
        if (open_) MMDB_close(&mmdb_);
    }

    // Returns country code, or "" on miss/error.
    std::string countryCode(const std::string& ip) const {
        std::unique_lock lk(mutex_);
        if (!open_) return "";
        int gai_err = 0, mmdb_err = 0;
        MMDB_lookup_result_s r = MMDB_lookup_string(&mmdb_, ip.c_str(), &gai_err, &mmdb_err);
        if (gai_err || mmdb_err != MMDB_SUCCESS || !r.found_entry) return "";
        MMDB_entry_data_s d;
        if (MMDB_get_value(&r.entry, &d, "country", "iso_code", nullptr) != MMDB_SUCCESS
                || !d.has_data
                || d.type != MMDB_DATA_TYPE_UTF8_STRING) return "";
        std::string cc(d.utf8_string, d.data_size);
        return cc;
    }

    // Hot-reload: open new handle first, then swap under exclusive lock.
    bool reload() {
        MMDB_s n{};
        if (MMDB_open(path_.c_str(), MMDB_MODE_MMAP, &n) != MMDB_SUCCESS) {
            std::cerr << "[GeoDatabase] reload failed: " << path_ << "\n";
            return false;
        }
        {
            std::unique_lock lk(mutex_);
            if (open_) MMDB_close(&mmdb_);
            mmdb_ = n;
            open_ = true;
            loaded_at_ = std::chrono::system_clock::now();
        }
        std::cout << "[GeoDatabase] reloaded: " << path_ << "\n";
        return true;
    }

    bool isOpen() const { return open_; }

    std::string loadedAt() const {
        std::unique_lock lk(mutex_);
        if (!open_) return "never";
        auto t = std::chrono::system_clock::to_time_t(loaded_at_);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
        return buf;
    }

private:
    std::string                            path_;
    MMDB_s                                 mmdb_{};
    std::atomic_bool                       open_{false};
    std::chrono::system_clock::time_point  loaded_at_;
    mutable std::mutex                     mutex_;
};

// DatabaseWatcher
// Runs in background; hot-reloads the MMDB when the file's mtime changes.
class DatabaseWatcher {
public:
    DatabaseWatcher(GeoDatabase& db, const std::string& path, int interval_sec)
        : db_(db), path_(path), interval_(interval_sec) {}

    void start() {
        try { last_mtime_ = fs::last_write_time(path_); } catch (...) {}
        thread_ = std::thread([this] {
            while (!stop_) {
                for (int i = 0; i < interval_ && !stop_; ++i)
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                if (stop_) break;
                try {
                    auto mt = fs::last_write_time(path_);
                    if (mt != last_mtime_) {
                        std::cout << "[DatabaseWatcher] change detected -- reloading\n";
                        if (db_.reload()) last_mtime_ = mt;
                    }
                } catch (const std::exception& ex) {
                    std::cerr << "[DatabaseWatcher] " << ex.what() << "\n";
                }
            }
        });
    }
    void stop() { stop_ = true; if (thread_.joinable()) thread_.join(); }

private:
    GeoDatabase&          db_;
    std::string           path_;
    int                   interval_;
    std::atomic_bool      stop_{false};
    std::thread           thread_;
    fs::file_time_type    last_mtime_{};
};

// Route handlers
static void onCheck(const httplib::Request& req,
                    httplib::Response&      res,
                    GeoDatabase&            geo)
{
    res.set_header("Content-Type", "application/json");

    if (!geo.isOpen()) {
        res.status = 503;
        res.body = "error:GeoIP database not loaded, code:503";
        return;
    }

    json body;
    try { body = json::parse(req.body); }
    catch (...) {
        res.status = 400;
        res.body = "error:Invalid JSON body, code:400";
        return;
    }

    if (!body.contains("ip") || !body["ip"].is_string()) {
        res.status = 400;
        res.body = "error:Missing or invalid field: ip, code:400";
        return;
    }
    if (!body.contains("allowed_countries") || !body["allowed_countries"].is_array()) {
        res.status = 400;
        res.body = "error:Missing or invalid field: allowed_countries, code:400";
        return;
    }

    const std::string ip = body["ip"].get<std::string>();

    std::unordered_set<std::string> allowed;
    for (const auto& c : body["allowed_countries"]) {
        if (c.is_string()) {
            std::string code = c.get<std::string>();
            for (auto& ch : code) ch = (char)std::toupper((unsigned char)ch);
            allowed.insert(code);
        }
    }

    if (allowed.empty()) {
        res.status = 200;
        res.body = json{{"allowed",false},{"country",""},{"ip",ip},
                        {"reason","allowed_countries list is empty"}}.dump();
        return;
    }

    const std::string country = geo.countryCode(ip);
    const bool ok = !country.empty() && allowed.count(country);
    json resp{{"allowed",ok},{"country",country},{"ip",ip}};
    if (country.empty()) resp["reason"] = "IP address not found in database";
    res.status = 200;
    res.body = resp.dump();
}

static void onReload(const httplib::Request&, httplib::Response& res, GeoDatabase& geo) {
    res.set_header("Content-Type", "application/json");
    bool ok = geo.reload();
    res.status = ok ? 200 : 500;
    res.body = json{{"reloaded", ok}}.dump();
}

// main 
int main(int argc, char *argv[]) {
    Config cfg;
    std::cout << "[geo-filter-service] host=" << cfg.host
              << " port=" << cfg.port
              << " mmdb=" << cfg.mmdb_path
              << " reload_interval=" << cfg.db_reload_interval_sec << "s\n";

    GeoDatabase     geo(cfg.mmdb_path);
    DatabaseWatcher watcher(geo, cfg.mmdb_path, cfg.db_reload_interval_sec);
    watcher.start();

    httplib::Server svr;
    svr.Post("/check",  [&](const httplib::Request& q, httplib::Response& r){ onCheck(q,r,geo); });
    svr.Post("/reload", [&](const httplib::Request& q, httplib::Response& r){ onReload(q,r,geo); });

    static httplib::Server* gSvr = &svr;
    std::signal(SIGTERM, [](int){ gSvr->stop(); });
    std::signal(SIGINT,  [](int){ gSvr->stop(); });

    std::cout << "[geo-filter-service] Listening on "
              << cfg.host << ":" << cfg.port << "\n";
    svr.listen(cfg.host.c_str(), cfg.port);

    watcher.stop();
    return 0;
}
