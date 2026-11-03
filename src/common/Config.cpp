#include "common/Config.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

namespace minisearch {

namespace {

template <typename T>
bool clampInto(T& target, const nlohmann::json& value, T minValue, T maxValue) {
    if (!value.is_number()) return false;
    target = value.get<T>();
    if (target < minValue) target = minValue;
    if (target > maxValue) target = maxValue;
    return true;
}

} // namespace

std::string EngineConfig::loadFromFile(const std::string& path, EngineConfig& out) {
    std::ifstream file(path);
    if (!file) return "";  // no config file: defaults stand

    std::ostringstream buffer;
    buffer << file.rdbuf();
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(buffer.str());
    } catch (const nlohmann::json::exception& error) {
        return std::string("invalid JSON in ") + path + ": " + error.what();
    }
    if (!root.is_object()) return path + ": top level must be an object";

    const auto section = [&root](const char* name) {
        const auto it = root.find(name);
        return it != root.end() && it->is_object() ? *it : nlohmann::json::object();
    };

    const nlohmann::json crawler = section("crawler");
    if (crawler.contains("max_pages")) clampInto(out.maxPages, crawler["max_pages"], 1, 5000);
    if (crawler.contains("max_depth")) clampInto(out.maxDepth, crawler["max_depth"], 0, 10);
    if (crawler.contains("delay_ms")) clampInto(out.delayMs, crawler["delay_ms"], 0, 60000);
    if (crawler.contains("request_timeout_seconds")) {
        clampInto(out.timeoutSeconds, crawler["request_timeout_seconds"], 1L, 120L);
    }
    if (crawler.contains("user_agent") && crawler["user_agent"].is_string()) {
        out.userAgent = crawler["user_agent"].get<std::string>();
    }

    const nlohmann::json text = section("text_processing");
    if (text.contains("remove_stopwords")) out.text.removeStopwords = text["remove_stopwords"].get<bool>();
    if (text.contains("apply_stemming")) out.text.applyStemming = text["apply_stemming"].get<bool>();
    if (text.contains("map_technical_terms")) {
        out.text.mapTechnicalTerms = text["map_technical_terms"].get<bool>();
    }

    const nlohmann::json ranking = section("ranking");
    if (ranking.contains("default_algorithm") && ranking["default_algorithm"].is_string()) {
        const std::string algorithm = ranking["default_algorithm"].get<std::string>();
        if (algorithm == "tfidf" || algorithm == "bm25") out.defaultAlgorithm = algorithm;
    }
    const nlohmann::json weights = ranking.contains("field_weights")
                                        ? ranking["field_weights"]
                                        : nlohmann::json::object();
    if (weights.contains("title")) clampInto(out.weights.title, weights["title"], 0.0, 100.0);
    if (weights.contains("heading")) clampInto(out.weights.heading, weights["heading"], 0.0, 100.0);
    if (weights.contains("body")) clampInto(out.weights.body, weights["body"], 0.0, 100.0);
    if (ranking.contains("bm25") && ranking["bm25"].is_object()) {
        const nlohmann::json& bm25 = ranking["bm25"];
        if (bm25.contains("k1")) clampInto(out.bm25K1, bm25["k1"], 0.0, 10.0);
        if (bm25.contains("b")) clampInto(out.bm25B, bm25["b"], 0.0, 1.0);
    }

    const nlohmann::json storage = section("storage");
    if (storage.contains("database_path") && storage["database_path"].is_string()) {
        out.dbPath = storage["database_path"].get<std::string>();
    }
    if (storage.contains("index_path") && storage["index_path"].is_string()) {
        out.indexPath = storage["index_path"].get<std::string>();
    }

    const nlohmann::json server = section("server");
    if (server.contains("host") && server["host"].is_string()) {
        out.host = server["host"].get<std::string>();
    }
    if (server.contains("port")) clampInto(out.port, server["port"], 1, 65535);
    if (server.contains("static_root") && server["static_root"].is_string()) {
        out.staticRoot = server["static_root"].get<std::string>();
    }
    if (server.contains("corpus_root") && server["corpus_root"].is_string()) {
        out.corpusRoot = server["corpus_root"].get<std::string>();
    }

    return "";
}

} // namespace minisearch
