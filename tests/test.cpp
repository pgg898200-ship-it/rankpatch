#include "../include/rankpatch.hpp"
#include "../include/rankpatch_steady.hpp"
#include "../bench/indexed.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using Item = std::pair<std::uint32_t, std::uint64_t>;
using List = std::vector<std::uint32_t>;

void require(bool ok, const std::string& reason) {
    if (!ok) throw std::runtime_error(reason);
}

struct Ballot {
    std::optional<std::uint64_t> revision;
    List ids;
};

// The oracle scores every active ballot from scratch, then sorts every item.
std::vector<Item> rebuild(const std::vector<Ballot>& ballots, const List& points,
                          std::size_t universe, std::size_t k) {
    std::vector<std::uint64_t> scores(universe);
    std::vector<bool> present(universe);
    for (const auto& ballot : ballots) {
        for (std::size_t pos = 0; pos < ballot.ids.size(); ++pos) {
            const auto id = ballot.ids[pos];
            present[id] = true;
            scores[id] += points[pos];
        }
    }
    std::vector<Item> items;
    for (std::size_t id = 0; id < scores.size(); ++id) {
        if (present[id]) items.emplace_back(id, scores[id]);
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    if (items.size() > k) items.resize(k);
    return items;
}

template <class Index>
void revisions_and_ties() {
    Index index(2, 8, {9, 4}, 3);
    require(index.top().empty(), "new index contains results");
    require(index.replace(0, 0, {6, 5}), "first revision zero rejected");
    require(index.replace(1, 11, {7, 2}), "second ballot rejected");
    require(index.top() == std::vector<Item>{{6, 9}, {7, 9}, {2, 4}},
            "tie at top-k boundary resolved incorrectly");
    require(!index.replace(1, 11, {}), "equal revision mutated ballot");
    require(!index.replace(1, 10, {3}), "older revision mutated ballot");
    require(index.replace(0, 1, {}), "retraction rejected");
    require(index.top() == std::vector<Item>{{7, 9}, {2, 4}}, "retraction left ghosts");
    require(index.replace(1, 12, {}), "final retraction rejected");
    require(index.top().empty(), "last retraction left active items");
    require(!index.replace(0, 0, {5}), "stale revision resurrected retracted ballot");
    const auto maxrev = std::numeric_limits<std::uint64_t>::max();
    require(index.replace(0, maxrev, {1}), "maximum revision rejected");
    require(!index.replace(0, 0, {}), "revision arithmetic wrapped");
    require(!index.replace(0, maxrev, {9, 9, 99}), "stale malformed ballot validated");
    require(index.top() == std::vector<Item>{{1, 9}}, "maximum revision lost");
}

template <class Index>
void rejection_is_atomic(const List& bad) {
    Index index(2, 8, {9, 4, 1}, 5);
    index.replace(0, 0, {6, 5, 4});
    index.replace(1, 4, {2, 4, 6});
    const auto before = index.top();
    bool rejected = false;
    try {
        rejected = !index.replace(0, 1, bad);
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "invalid ballot accepted");
    require(index.top() == before, "invalid ballot changed visible scores");
    require(index.replace(0, 1, {3}), "invalid ballot advanced revision");
    require(index.top() == std::vector<Item>{{2, 9}, {3, 9}, {4, 4}, {6, 1}},
            "invalid ballot changed retirement or scoring state");
}

template <class Index>
void wide_scores() {
    const auto p = std::numeric_limits<std::uint32_t>::max();
    Index index(3, 8, {p}, 1);
    for (std::size_t s = 0; s < 3; ++s) index.replace(s, 0, {7});
    require(index.top() == std::vector<Item>{{7, std::uint64_t(p) * 3}}, "sum overflow");
    index.replace(1, 1, {});
    index.replace(0, 1, {1});
    require(index.top() == std::vector<Item>{{1, p}}, "overflow after removal");
}

template <class Index>
void promote_untouched_runners_up() {
    constexpr std::size_t n = 4099;
    // Spread tied candidates over a sparse, non-power-of-two ID space.
    Index index(64, n, {13, 7, 1}, 7);
    std::vector<Ballot> ballots(64);
    auto set = [&](std::size_t source, std::uint64_t rev, List ids) {
        require(index.replace(source, rev, ids), "fresh sparse update rejected");
        ballots[source] = {rev, std::move(ids)};
        require(index.top() == rebuild(ballots, {13, 7, 1}, n, 7),
                "winner promotion failed for source " + std::to_string(source));
    };
    for (unsigned s = 0; s < 64; ++s) set(s, 0, {64 * s, 4098, 4097});
    for (unsigned s = 0; s < 64; ++s) set(s, 1, {});
    for (unsigned s = 64; s-- > 0;) set(s, 2, {64 * s + 1});
}

void randomized(std::size_t k, std::uint32_t seed, int read_interval) {
    constexpr std::size_t sources = 37;
    constexpr std::size_t universe = 113;
    const List points = {211, 151, 97, 97, 58, 45, 30, 14, 14, 5, 2, 1};
    rankpatch::Index index(sources, universe, points, k);
    rankpatch::SteadyIndex steady(sources, universe, points, k);
    IndexedHeap contender(sources, universe, points, k);
    std::vector<Ballot> ballots(sources);
    std::mt19937 rng(seed);
    List population(universe);
    std::iota(population.begin(), population.end(), 0u);

    for (int step = 0; step < 4000; ++step) {
        const auto source = rng() % sources;
        auto& ballot = ballots[source];
        auto rev = ballot.revision.value_or(0);
        if (ballot.revision) {
            switch (rng() % 6) {
                case 0: break;                             // duplicate event
                case 1: if (rev > 0) --rev; break;         // older event
                default: rev += 1 + rng() % 13; break;     // fresh event
            }
        }
        List next;
        switch (rng() % 5) {
            case 0: break;  // empty snapshot or retraction
            case 1: next = ballot.ids; break;
            case 2: next.assign(ballot.ids.rbegin(), ballot.ids.rend()); break;
            default: {
                std::shuffle(population.begin(), population.end(), rng);
                const auto len = 1 + rng() % points.size();
                next.assign(population.begin(), population.begin() + len);
                break;
            }
        }
        const bool fresh = !ballot.revision || rev > *ballot.revision;
        const auto context = " in seed " + std::to_string(seed) + " step " + std::to_string(step);
        require(index.replace(source, rev, next) == fresh, "wrong revision result" + context);
        require(steady.replace(source, rev, next) == fresh, "steady revision mismatch" + context);
        require(contender.replace(source, rev, next) == fresh, "heap revision mismatch" + context);
        if (fresh) ballot = {rev, std::move(next)};
        if ((step + 1) % read_interval == 0 || step == 3999) {
            const auto expected = rebuild(ballots, points, universe, k);
            require(index.top() == expected, "oracle mismatch" + context);
            require(steady.top() == expected, "steady oracle mismatch" + context);
            require(contender.top() == expected, "heap oracle mismatch" + context);
        }
    }
}

template <class Index>
void swap_and_follow_on() {
    Index index(1, 8, {13, 7, 7}, 99);
    index.replace(0, 1, {6, 5, 4});
    require(index.replace(0, 2, {6, 4, 5}), "equal-points swap rejected");
    require(index.top() == std::vector<Item>{{6, 13}, {4, 7}, {5, 7}}, "equal-point swap");
    require(index.replace(0, 3, {4, 6, 5}), "unequal-points follow-on rejected");
    require(index.top() == std::vector<Item>{{4, 13}, {5, 7}, {6, 7}}, "follow-on swap");
    require(index.replace(0, 4, {}), "follow-on retraction rejected");
    require(index.top().empty(), "swap followed by retraction left items");
}

template <class Index>
void regression_suite() {
    revisions_and_ties<Index>();
    rejection_is_atomic<Index>({1, 3, 1});
    rejection_is_atomic<Index>({1, 3, 8});
    rejection_is_atomic<Index>({1, 2, 3, 4});
    rejection_is_atomic<Index>({5, 6, 5});
    rejection_is_atomic<Index>({5, 6, 8});
    swap_and_follow_on<Index>();
    wide_scores<Index>();
    promote_untouched_runners_up<Index>();
}

int main() {
    try {
        regression_suite<rankpatch::Index>();
        regression_suite<rankpatch::SteadyIndex>();
        randomized(1, 79591, 1);
        randomized(12, 113011, 1);
        randomized(113, 913807, 1);
        randomized(7, 914947, 31);
        randomized(0, 917123, 1);
        std::cout << "Passed regression cases and 20000 randomized updates.\n";
    } catch (const std::exception& ex) {
        std::cerr << "FAILED: " << ex.what() << '\n';
        return 1;
    }
}
