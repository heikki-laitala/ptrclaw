#include <catch2/catch_test_macros.hpp>
#include "memory/response_cache.hpp"
#include <filesystem>
#include <thread>
#include <chrono>
#include <unistd.h>

using namespace ptrclaw;

static std::string cache_test_path() {
    return "/tmp/ptrclaw_test_cache_" + std::to_string(getpid()) + ".json";
}

struct CacheFixture {
    std::string path = cache_test_path();
    ResponseCache cache{path, 3600, 100};

    ~CacheFixture() {
        std::filesystem::remove(path);
        std::filesystem::remove(path + ".tmp");
    }
};

// ── Basic get/put ────────────────────────────────────────────

TEST_CASE("ResponseCache: miss on empty cache", "[cache]") {
    CacheFixture f;
    auto result = f.cache.get("model", "sys", "hello");
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("ResponseCache: hit after put", "[cache]") {
    CacheFixture f;
    f.cache.put("model", "sys", "hello", "world");

    auto result = f.cache.get("model", "sys", "hello");
    REQUIRE(result.has_value());
    REQUIRE(result.value_or("") == "world");
}

TEST_CASE("ResponseCache: different input misses", "[cache]") {
    CacheFixture f;
    f.cache.put("model", "sys", "hello", "world");

    auto result = f.cache.get("model", "sys", "goodbye");
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("ResponseCache: different model misses", "[cache]") {
    CacheFixture f;
    f.cache.put("gpt-4", "sys", "hello", "world");

    auto result = f.cache.get("gpt-3", "sys", "hello");
    REQUIRE_FALSE(result.has_value());
}

// ── Size tracking ────────────────────────────────────────────

TEST_CASE("ResponseCache: size increases with entries", "[cache]") {
    CacheFixture f;
    REQUIRE(f.cache.size() == 0);

    f.cache.put("m", "s", "q1", "r1");
    REQUIRE(f.cache.size() == 1);

    f.cache.put("m", "s", "q2", "r2");
    REQUIRE(f.cache.size() == 2);
}

TEST_CASE("ResponseCache: clear empties cache", "[cache]") {
    CacheFixture f;
    f.cache.put("m", "s", "q", "r");
    REQUIRE(f.cache.size() == 1);

    f.cache.clear();
    REQUIRE(f.cache.size() == 0);
}

// ── LRU eviction ─────────────────────────────────────────────

TEST_CASE("ResponseCache: LRU eviction at max_entries", "[cache]") {
    std::string path = cache_test_path() + "_lru";
    ResponseCache cache(path, 3600, 3); // max 3 entries

    cache.put("m", "s", "q1", "r1");
    cache.put("m", "s", "q2", "r2");
    cache.put("m", "s", "q3", "r3");
    REQUIRE(cache.size() == 3);

    // Adding a 4th should evict one to stay at max_entries
    cache.put("m", "s", "q4", "r4");
    REQUIRE(cache.size() <= 3);

    // At least some entries should still be accessible
    int found = 0;
    if (cache.get("m", "s", "q1").has_value()) found++;
    if (cache.get("m", "s", "q2").has_value()) found++;
    if (cache.get("m", "s", "q3").has_value()) found++;
    if (cache.get("m", "s", "q4").has_value()) found++;
    REQUIRE(found >= 2); // At least 2 of 4 should remain

    std::filesystem::remove(path);
}

// ── TTL expiry ───────────────────────────────────────────────

TEST_CASE("ResponseCache: TTL expiry", "[cache]") {
    std::string path = cache_test_path() + "_ttl";
    ResponseCache cache(path, 1, 100); // 1 second TTL

    cache.put("m", "s", "q", "r");
    REQUIRE(cache.get("m", "s", "q").has_value());

    // Wait for TTL to expire
    std::this_thread::sleep_for(std::chrono::seconds(2));

    auto result = cache.get("m", "s", "q");
    REQUIRE_FALSE(result.has_value());

    std::filesystem::remove(path);
}

// ── Persistence ──────────────────────────────────────────────

TEST_CASE("ResponseCache: persists across instances", "[cache]") {
    std::string path = cache_test_path() + "_persist";

    {
        ResponseCache cache(path, 3600, 100);
        cache.put("m", "s", "q", "r");
    }

    {
        ResponseCache cache(path, 3600, 100);
        auto result = cache.get("m", "s", "q");
        REQUIRE(result.has_value());
        REQUIRE(result.value_or("") == "r");
    }

    std::filesystem::remove(path);
}
