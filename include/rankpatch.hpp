#ifndef RANKPATCH_HPP
#define RANKPATCH_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace rankpatch {

// A single-owner index over dense document IDs [0, universe). One replacement
// installs one source's entire ballot; the weight at position i is points[i].
// top() returns active documents by descending integer score, then ascending ID.
// Borrowed results are current until the next mutation. Synchronize shared use.
class Index {
public:
    using Row = std::pair<std::uint32_t, std::uint64_t>;

    Index(std::size_t sources, std::size_t universe,
          std::vector<std::uint32_t> rank_points, std::size_t k)
        : points_(std::move(rank_points)), k_(std::min(k, universe)) {
        if (universe && universe - 1 > std::numeric_limits<std::uint32_t>::max())
            throw std::invalid_argument("document universe exceeds uint32 IDs");
        std::uint32_t maximum = 0;
        for (auto point : points_) {
            if (!point) throw std::invalid_argument("rank points must be positive");
            maximum = std::max(maximum, point);
        }
        if (maximum && sources > std::numeric_limits<std::uint64_t>::max() / maximum)
            throw std::overflow_error("source count times maximum rank points overflows");

        const auto depth = std::min(universe, points_.size());
        const auto capacity = depth && sources > universe / depth ? universe : sources * depth;
        // The extra candidates absorb demotions at the kth position. This is
        // only a performance parameter; changing it does not change results.
        guard_target_ = k_ > capacity / 4 ? capacity : 4 * k_;
        sources_.resize(sources);
        scores_.resize(universe);
        incoming_.resize(universe);
        active_pos_.resize(universe);
        guard_pos_.resize(universe);
        active_.reserve(capacity);
        if (k_) guard_.reserve(capacity);
        result_.reserve(std::min(k_, capacity));
    }

    bool is_fresh(std::size_t source, std::uint64_t revision) const {
        if (source >= sources_.size()) throw std::out_of_range("source ID");
        const auto& ballot = sources_[source];
        return !ballot.seen || revision > ballot.revision;
    }

    // Revision zero is valid. Strictly older revisions and replays are ignored
    // before payload validation. Rejected fresh ballots leave all scores and
    // the source revision intact. O(old depth + new depth); new sources may
    // allocate ballot storage. Ranking work can be deferred to the next read.
    bool replace(std::size_t source, std::uint64_t revision,
                 const std::vector<std::uint32_t>& ids) {
        if (!is_fresh(source, revision)) return false;
        auto& s = sources_[source];
        if (ids.size() > points_.size()) throw std::invalid_argument("ballot too long");
        if (ids.size() == s.ids.size()) {
            std::size_t i = 0;
            while (i < ids.size() && ids[i] == s.ids[i]) ++i;
            if (i == ids.size()) {
                s.seen = true;
                s.revision = revision;
                return true;
            }
            // A reversal of exactly two positions preserves the validated set
            // of IDs. Verify the suffix before updating either score.
            if (i + 1 < ids.size() && ids[i] == s.ids[i + 1] &&
                ids[i + 1] == s.ids[i] &&
                std::equal(ids.begin() + i + 2, ids.end(), s.ids.begin() + i + 2)) {
                if (points_[i] != points_[i + 1]) {
                    change(s.ids[i], points_[i], points_[i + 1]);
                    change(s.ids[i + 1], points_[i + 1], points_[i]);
                }
                std::swap(s.ids[i], s.ids[i + 1]);
                s.revision = revision;
                return true;
            }
        }
        s.ids.reserve(ids.size());
        // incoming_ is both the uniqueness check and a direct map from a
        // new document to its points. Zero is free between replacements.
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const auto id = ids[i];
            if (id >= scores_.size() || incoming_[id]) {
                for (std::size_t j = 0; j < i; ++j) incoming_[ids[j]] = 0;
                if (id >= scores_.size()) throw std::out_of_range("document ID");
                throw std::invalid_argument("duplicate document ID in ballot");
            }
            incoming_[id] = points_[i];
        }
        // A document moving to another position is touched once after its
        // old and new points are known. Removals precede new insertions.
        for (std::size_t i = 0; i < s.ids.size(); ++i) {
            const auto id = s.ids[i];
            const auto next = incoming_[id];
            incoming_[id] = 0;
            if (points_[i] != next) change(id, points_[i], next);
        }
        for (auto id : ids) {
            if (const auto next = incoming_[id]) {
                incoming_[id] = 0;
                change(id, 0, next);
            }
        }
        s.ids.assign(ids.begin(), ids.end());
        s.seen = true;
        s.revision = revision;
        return true;
    }

    // Cached reads are O(1). An invalidated read selects among C guard
    // candidates in expected O(C + k log k). If the guard is depleted, C
    // becomes the number of active documents. This fallback can occur on
    // every read for adversarial replacements. Retained memory is O(universe
    // + sources + total ballot capacities + rank_points.size()).
    const std::vector<Row>& top() {
        if (!dirty_ || !k_) return result_;
        const auto count = std::min(k_, active_.size());
        const bool rebuild = !ready_ || guard_.size() < count;
        const auto& pool = rebuild ? active_ : guard_;
        scratch_.clear();
        scratch_.reserve(pool.size());
        for (auto id : pool) scratch_.emplace_back(id, scores_[id]);

        if (rebuild || guard_.size() - std::min(guard_.size(), guard_target_) > guard_target_) {
            // Every outsider ranks strictly below the cutoff (score, ID).
            // When trimming an existing guard, its new cutoff can only rise.
            if (scratch_.size() > guard_target_) {
                auto keep = scratch_.begin() + guard_target_;
                std::nth_element(scratch_.begin(), keep, scratch_.end(), better);
                scratch_.resize(guard_target_);
                cutoff_ = *std::max_element(scratch_.begin(), scratch_.end(), better);
            } else {
                // This branch occurs only on a full rebuild, which has seen
                // all active IDs. A zero cutoff admits every subsequent arrival.
                cutoff_ = {0, 0};
            }
            guard_.clear();
            for (const auto& row : scratch_) insert(guard_, guard_pos_, row.first);
            ready_ = true;
        }
        if (count < scratch_.size()) {
            std::nth_element(scratch_.begin(), scratch_.begin() + count, scratch_.end(), better);
            scratch_.resize(count);
        }
        std::sort(scratch_.begin(), scratch_.end(), better);
        result_.assign(scratch_.begin(), scratch_.end());
        dirty_ = false;
        return result_;
    }

private:
    struct Ballot {
        std::vector<std::uint32_t> ids;
        std::uint64_t revision = 0;
        bool seen = false;
    };

    static bool better(const Row& a, const Row& b) {
        return a.second > b.second || (a.second == b.second && a.first < b.first);
    }

    static void insert(std::vector<std::uint32_t>& set,
                       std::vector<std::uint32_t>& pos, std::uint32_t id) {
        pos[id] = static_cast<std::uint32_t>(set.size());
        set.push_back(id);
    }

    static void erase(std::vector<std::uint32_t>& set,
                      std::vector<std::uint32_t>& pos, std::uint32_t id) {
        const auto tail = set.back();
        set[pos[id]] = tail;
        pos[tail] = pos[id];
        set.pop_back();
    }

    bool admitted(const Row& row) const {
        return row.second && !better(cutoff_, row);
    }

    void change(std::uint32_t id, std::uint32_t previous, std::uint32_t next) {
        const Row before{id, scores_[id]};
        const Row after{id, before.second - previous + next};
        scores_[id] = after.second;
        if (!before.second) insert(active_, active_pos_, id);
        if (!after.second) erase(active_, active_pos_, id);
        if (!k_ || !ready_) return;
        if (!dirty_) {
            // A cached winner changed, or a non-winner crossed its boundary.
            // The full (score, ID) ordering also resolves ties at the boundary.
            dirty_ = result_.size() < k_ || !better(result_.back(), before)
                     || better(after, result_.back());
        }
        const bool was = admitted(before);
        const bool now = admitted(after);
        if (was && !now) erase(guard_, guard_pos_, id);
        if (!was && now) insert(guard_, guard_pos_, id);
    }

    std::vector<std::uint32_t> points_;
    std::size_t k_;
    std::size_t guard_target_ = 0;
    std::vector<Ballot> sources_;
    std::vector<std::uint64_t> scores_;
    std::vector<std::uint32_t> incoming_, active_pos_, guard_pos_, active_, guard_;
    std::vector<Row> result_, scratch_;
    Row cutoff_{0, 0};
    bool ready_ = false;
    bool dirty_ = true;
};

} // namespace rankpatch
#endif
