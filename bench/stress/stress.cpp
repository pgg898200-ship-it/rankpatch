#include "../../include/rankpatch.hpp"
#include "../../include/rankpatch_steady.hpp"
#include "../indexed.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <utility>
#include <vector>

namespace {
using Id = std::uint32_t;
using Score = std::uint64_t;
using Ballot = std::vector<Id>;
using Top = std::vector<std::pair<Id, Score>>;
using Clock = std::chrono::steady_clock;
constexpr std::size_t sources = 65536, depth = 16, universe = sources * depth, k = 10;
constexpr std::size_t events = 4096, checkpoint_every = 256;
constexpr std::uint64_t seed = 20261122;

class Oracle {
public:
    Oracle(std::size_t n, std::size_t, const Ballot& points, std::size_t)
        : ballots_(n), points_(points) {}
    bool replace(std::size_t s, std::uint64_t, const Ballot& ids) {
        ballots_[s] = ids;
        return true;
    }
    Top top() const {
        std::vector<Score> scores(universe);
        for (const auto& ballot : ballots_)
            for (std::size_t i = 0; i < ballot.size(); ++i)
                scores[ballot[i]] += points_[i];
        Top rows;
        for (std::size_t i = 0; i < universe; ++i)
            if (scores[i]) rows.emplace_back(static_cast<Id>(i), scores[i]);
        auto count = std::min(k, rows.size());
        std::partial_sort(rows.begin(), rows.begin()+count, rows.end(), [](auto a, auto b) {
            return a.second != b.second ? a.second > b.second : a.first < b.first;
        });
        rows.resize(count);
        return rows;
    }
private:
    std::vector<Ballot> ballots_;
    Ballot points_;
};

Ballot random_ballot(std::mt19937_64& gen) {
    Ballot b;
    b.reserve(depth);
    while (b.size() < depth) {
        // Universe is a power of two. Masking gives one uniform ID per generator draw.
        const auto id = static_cast<Id>(gen() & (universe-1));
        if (std::find(b.begin(), b.end(), id) == b.end()) b.push_back(id);
    }
    return b;
}

std::uint64_t hash(const Top& rows) {
    std::uint64_t h = 14695981039346656037ULL;
    for (auto row : rows) {
        h = (h ^ row.first) * 1099511628211ULL;
        h = (h ^ row.second) * 1099511628211ULL;
    }
    return h;
}

template<class Engine, bool sampling = false>
void run(bool withdrawal) {
    Ballot points(depth, 1);
    if (!withdrawal)
        for (std::size_t i = 0; i < depth; ++i) points[i] = 1000000u/(61+i);
    Engine engine(sources, universe, points, k);
    std::vector<std::uint64_t> revisions(sources,1);
    std::mt19937_64 gen(seed);
    for (std::size_t s = 0; s < sources; ++s) {
        Ballot b;
        if (withdrawal) {
            b.resize(depth);
            std::iota(b.begin(), b.end(), static_cast<Id>(s*depth));
        } else {
            b = random_ballot(gen);
        }
        engine.replace(s,1,b);
    }
    (void)engine.top();
    std::cout << "event,ns,hash\n";
    for (std::size_t i = 1; i <= events; ++i) {
        const auto source = withdrawal ? i-1 : (gen() & (sources-1));
        const auto ids = withdrawal ? Ballot{} : random_ballot(gen);
        const auto revision = ++revisions[source];
        const auto begin = Clock::now();
        if (!engine.replace(source, revision, ids)) throw std::runtime_error("rejected fresh update");
        if constexpr (sampling) {
            if (i!=1 && i%checkpoint_every != 0) continue;
        }
        const auto& top = engine.top();
        const auto end = Clock::now();
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end-begin).count();
        std::cout << i << ',' << ns << ',' << hash(top) << '\n';
    }
    rusage usage{};
    if (getrusage(RUSAGE_SELF,&usage)) throw std::runtime_error("unable to read peak process RSS");
    std::cerr << usage.ru_maxrss << '\n'; // Linux units are KiB.
}
} // namespace

int main(int argc, char** argv) try {
    if (argc != 3) throw std::invalid_argument("expected ENGINE SCENARIO");
    const std::string engine(argv[1]), scenario(argv[2]);
    const bool withdrawal = scenario=="withdraw";
    if (!withdrawal && scenario!="replace") throw std::invalid_argument("unknown scenario");
    if (engine=="rankpatch") run<rankpatch::Index>(withdrawal);
    else if (engine=="steady") run<rankpatch::SteadyIndex>(withdrawal);
    else if (engine=="heap") run<IndexedHeap>(withdrawal);
    else if (engine=="oracle") run<Oracle,true>(withdrawal);
    else throw std::invalid_argument("unknown engine");
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
