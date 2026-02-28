#pragma once
#include "../memory.hpp"
#include "../embedder.hpp"
#include "../config.hpp"
#include <mutex>
#include <random>
#include <string>

namespace ptrclaw {

// Shared base for memory backends that support embeddings and decay.
// NoneMemory inherits from Memory directly (no state needed).
class BaseMemory : public Memory {
protected:
    std::string path_;
    mutable std::mutex mutex_;

    Embedder* embedder_ = nullptr;
    double text_weight_ = 0.4;
    double vector_weight_ = 0.6;
    uint32_t recency_half_life_ = 0;
    uint32_t knowledge_max_idle_days_ = 0;
    double knowledge_survival_chance_ = 0.05;
    std::mt19937 rng_{std::random_device{}()};
    std::uniform_real_distribution<double> dist_{0.0, 1.0};

public:
    void set_embedder(Embedder* embedder, double tw = 0.4,
                      double vw = 0.6) override {
        embedder_ = embedder;
        text_weight_ = tw;
        vector_weight_ = vw;
    }

    void set_recency_decay(uint32_t half_life_seconds) override {
        recency_half_life_ = half_life_seconds;
    }

    void set_knowledge_decay(uint32_t max_idle_days,
                             double survival_chance) override {
        knowledge_max_idle_days_ = max_idle_days;
        knowledge_survival_chance_ = survival_chance;
    }

    void apply_config(const MemoryConfig& cfg) override {
        set_recency_decay(cfg.recency_half_life);
        set_knowledge_decay(cfg.knowledge_max_idle_days, cfg.knowledge_survival_chance);
    }
};

} // namespace ptrclaw
