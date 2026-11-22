// Compile from the project root with: c++ -std=c++17 -O3 -DNDEBUG bench/bench.cpp -o bench-rankpatch
#include "../include/rankpatch.hpp"
#include "indexed.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using Id = std::uint32_t;
using Score = std::uint64_t;
using Ballot = std::vector<Id>;
using Entry = std::pair<Id, Score>;
using Top = std::vector<Entry>;
using Clock = std::chrono::steady_clock;

struct Better {
    bool operator()(const Entry& lhs, const Entry& rhs) const {
        return lhs.second != rhs.second ? lhs.second > rhs.second : lhs.first < rhs.first;
    }
};

constexpr std::size_t depth = 32;
constexpr std::size_t winners = 10;

struct Workload {
    std::size_t sources;
    std::size_t universe;
    bool high_overlap;
    std::string change;
};
struct Update {
    std::size_t source;
    std::uint64_t revision;
    Ballot ids;
};
struct Replay {
    std::vector<Ballot> initial;
    std::vector<Update> updates;
};

// Active-only clearing avoids an O(universe) scan. Partial sort only orders the requested prefix.
class Rebuild {
public:
    Rebuild(std::size_t sources, std::size_t universe, const Ballot& points, std::size_t k)
        : ballots_(sources), revisions_(sources), points_(points), scores_(universe), k_(k) {
        active_.reserve(universe);
        entries_.reserve(universe);
    }
    bool replace(std::size_t source, std::uint64_t revision, const Ballot& ids) {
        if (revision <= revisions_[source]) return false;
        ballots_[source] = ids;
        revisions_[source] = revision;
        return true;
    }
    const Top& top() {
        active_.clear();
        for (const auto& ballot : ballots_) {
            for (std::size_t i = 0; i < ballot.size(); ++i) {
                const auto id = ballot[i];
                if (scores_[id] == 0) active_.push_back(id);
                scores_[id] += points_[i];
            }
        }
        entries_.clear();
        for (const auto id : active_) {
            entries_.emplace_back(id, scores_[id]);
            scores_[id] = 0;
        }
        const auto n = std::min(k_, entries_.size());
        std::partial_sort(entries_.begin(), entries_.begin() + n, entries_.end(), Better{});
        entries_.resize(n);
        return entries_;
    }
private:
    std::vector<Ballot> ballots_;
    std::vector<std::uint64_t> revisions_;
    Ballot points_;
    std::vector<Score> scores_;
    std::size_t k_;
    Ballot active_;
    Top entries_;
};

// Net all position changes per ID. For a changed live ID, update an extracted
// tree node and reinsert it, avoiding an allocation and a key-based erase.
class TreeIndex {
    using Tree = std::set<Entry, Better>;
public:
    TreeIndex(std::size_t sources, std::size_t universe, const Ballot& points, std::size_t k)
        : ballots_(sources), revisions_(sources), points_(points), scores_(universe),
          delta_(universe), marked_(universe), locations_(universe), k_(k) {
        touched_.reserve(2 * points_.size());
        result_.reserve(k_);
    }
    bool replace(std::size_t source, std::uint64_t revision, const Ballot& ids) {
        if (revision <= revisions_[source]) return false;
        const auto& old = ballots_[source];
        touched_.clear();
        for (std::size_t i = 0; i < std::max(old.size(), ids.size()); ++i) {
            if (i < old.size() && i < ids.size() && old[i] == ids[i]) continue;
            const auto points = static_cast<std::int64_t>(points_[i]);
            if (i < old.size()) change(old[i], -points);
            if (i < ids.size()) change(ids[i], points);
        }
        for (const auto id : touched_) {
            marked_[id] = false;
            const auto difference = delta_[id];
            delta_[id] = 0;
            if (difference == 0) continue;
            const Score before = scores_[id];
            const Score after = static_cast<Score>(static_cast<std::int64_t>(before) + difference);
            if (before && after) {
                auto node = tree_.extract(locations_[id]);
                node.value().second = after;
                locations_[id] = tree_.insert(std::move(node)).position;
            } else if (before) {
                tree_.erase(locations_[id]);
            } else {
                locations_[id] = tree_.emplace(id, after).first;
            }
            scores_[id] = after;
        }
        ballots_[source] = ids;
        revisions_[source] = revision;
        return true;
    }
    const Top& top() {
        result_.clear();
        for (auto it = tree_.begin(); it != tree_.end() && result_.size() < k_; ++it)
            result_.push_back(*it);
        return result_;
    }
private:
    void change(Id id, std::int64_t delta) {
        if (!marked_[id]) {
            marked_[id] = true;
            touched_.push_back(id);
        }
        delta_[id] += delta;
    }
    std::vector<Ballot> ballots_;
    std::vector<std::uint64_t> revisions_;
    Ballot points_;
    std::vector<Score> scores_;
    std::vector<std::int64_t> delta_;
    std::vector<std::uint8_t> marked_;
    Tree tree_;
    std::vector<Tree::iterator> locations_;
    std::size_t k_;
    Ballot touched_;
    Top result_;
};

Ballot random_ballot(std::mt19937_64& rng, std::size_t universe) {
    std::uniform_int_distribution<Id> draw(0, static_cast<Id>(universe - 1));
    Ballot ballot;
    ballot.reserve(depth);
    while (ballot.size() < depth) {
        const auto id = draw(rng);
        if (std::find(ballot.begin(), ballot.end(), id) == ballot.end()) ballot.push_back(id);
    }
    return ballot;
}

Replay generate(const Workload& w, std::size_t events, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    Replay replay;
    for (std::size_t s = 0; s < w.sources; ++s) replay.initial.push_back(random_ballot(rng, w.universe));
    auto current = replay.initial;
    std::vector<std::uint64_t> revisions(w.sources, 1);
    std::uniform_int_distribution<std::size_t> choose_source(0, w.sources - 1);
    std::uniform_int_distribution<std::size_t> choose_rank(0, depth - 2);
    replay.updates.reserve(events);
    for (std::size_t i = 0; i < events; ++i) {
        const auto s = choose_source(rng);
        auto& ballot = current[s];
        if (w.change == "swap") {
            const auto pos = choose_rank(rng);
            std::swap(ballot[pos], ballot[pos + 1]);
        } else if (w.change == "retract_restore" && !ballot.empty()) {
            ballot.clear();
        } else {
            ballot = random_ballot(rng, w.universe);
        }
        replay.updates.push_back({s, ++revisions[s], ballot});
    }
    return replay;
}

template <typename Engine>
void warm(Engine& engine, const Replay& r) {
    for (std::size_t s = 0; s < r.initial.size(); ++s) {
        if (!engine.replace(s, 1, r.initial[s])) throw std::runtime_error("initial write rejected");
    }
    (void)engine.top();
}

std::vector<Top> expected(const Workload& w, const Ballot& points, const Replay& r) {
    Rebuild engine(w.sources, w.universe, points, winners);
    warm(engine, r);
    std::vector<Top> tops;
    tops.reserve(r.updates.size());
    for (const auto& u : r.updates) {
        engine.replace(u.source, u.revision, u.ids);
        tops.push_back(engine.top());
    }
    return tops;
}

struct Result {
    double p50_ns;
    double p99_ns;
    double ops_per_second;
    std::uint64_t checksum;
};

template <typename Engine>
Result trial(const Workload& w, const Ballot& points, const Replay& r, const std::vector<Top>& oracle) {
    Engine engine(w.sources, w.universe, points, winners);
    warm(engine, r);
    std::vector<double> elapsed(r.updates.size());
    std::uint64_t checksum = 14695981039346656037ULL;
    for (std::size_t i = 0; i < r.updates.size(); ++i) {
        const auto& u = r.updates[i];
        const auto start = Clock::now();
        const bool accepted = engine.replace(u.source, u.revision, u.ids);
        const auto& result = engine.top();
        const auto end = Clock::now();
        if (!accepted || result != oracle[i]) throw std::runtime_error("result mismatch at update " + std::to_string(i));
        elapsed[i] = std::chrono::duration<double, std::nano>(end - start).count();
        for (const auto& item : result) {
            checksum = (checksum ^ item.first) * 1099511628211ULL;
            checksum = (checksum ^ item.second) * 1099511628211ULL;
        }
    }
    const auto total = std::accumulate(elapsed.begin(), elapsed.end(), 0.0);
    std::sort(elapsed.begin(), elapsed.end());
    auto percentile = [&](double quantile) {
        return elapsed[static_cast<std::size_t>(std::ceil(quantile * elapsed.size())) - 1];
    };
    return {percentile(0.50), percentile(0.99), 1e9 * elapsed.size() / total, checksum};
}

std::uint64_t unsigned_value(const std::string& raw) {
    if (raw.empty() || raw.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("expected unsigned integer: " + raw);
    return std::stoull(raw);
}
} // namespace

int main(int argc, char** argv) try {
    std::size_t events = 256;
    std::size_t repeats = 3;
    std::uint64_t base_seed = 20261122;
    for (int i = 1; i < argc; ++i) {
        const std::string flag(argv[i]);
        if (flag == "--help") {
            std::cout << "bench-rankpatch [--events N] [--repeats N] [--seed N]\n";
            return 0;
        }
        if (i + 1 == argc) throw std::invalid_argument("missing value for " + flag);
        const auto value = unsigned_value(argv[++i]);
        if (flag == "--events") events = value;
        else if (flag == "--repeats") repeats = value;
        else if (flag == "--seed") base_seed = value;
        else throw std::invalid_argument("unknown flag " + flag);
    }
    if (!events || !repeats) throw std::invalid_argument("events and repeats must be positive");
    Ballot points(depth);
    for (std::size_t i = 0; i < depth; ++i) points[i] = 1000000u / (61 + i);
    std::cout << "sources,universe,depth,k,overlap,update,trial,seed,events,implementation,p50_ns,p99_ns,ops_per_second,checksum\n";
    std::cout << std::fixed << std::setprecision(2);
    const std::array<const char*, 4> names = {"rankpatch", "ordered_set", "full_rebuild", "indexed_heap"};
    std::size_t scenario = 0;
    for (const auto sources : {4u, 64u, 1024u}) {
        for (const bool overlap : {false, true}) {
            for (const auto change : {"swap", "replace", "retract_restore"}) {
                const Workload w{sources, overlap ? 96u : 128u * sources, overlap, change};
                for (std::size_t rep = 0; rep < repeats; ++rep) {
                    const std::uint64_t seed = base_seed + scenario * 100003ULL + rep;
                    const auto replay = generate(w, events, seed);
                    const auto reference = expected(w, points, replay);
                    // Rotate order to reduce systematic first-run bias.
                    for (std::size_t j = 0; j < names.size(); ++j) {
                        const auto which = (j + rep + scenario) % names.size();
                        Result result{};
                        if (which == 0) result = trial<rankpatch::Index>(w, points, replay, reference);
                        else if (which == 1) result = trial<TreeIndex>(w, points, replay, reference);
                        else if (which == 2) result = trial<Rebuild>(w, points, replay, reference);
                        else result = trial<IndexedHeap>(w, points, replay, reference);
                        std::cout << w.sources << ',' << w.universe << ',' << depth << ',' << winners << ','
                                  << (w.high_overlap ? "high" : "dilute") << ',' << w.change << ',' << rep + 1
                                  << ',' << seed << ',' << events << ',' << names[which] << ',' << result.p50_ns
                                  << ',' << result.p99_ns << ',' << result.ops_per_second << ',' << result.checksum << '\n';
                    }
                }
                ++scenario;
            }
        }
    }
    return 0;
} catch (const std::exception& e) {
    std::cerr << "benchmark failed: " << e.what() << '\n';
    return 1;
}
