#include "anti_entropy.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
                   #cond);                                               \
      ++failures;                                                        \
    }                                                                    \
  } while (0)

sy::Entry at(const std::string& value, std::int64_t wall) {
  return {value, {wall, 0}, {wall, 0}};
}

// The whole exchange with the network taken out: downstream asks, upstream
// answers, until the walk says it is done.
sy::Walk::Stats sync(const sy::Store& up, sy::Store& down,
                     std::vector<std::string>* applied = nullptr) {
  sy::Walk walk;
  std::vector<std::string> keys;
  sy::Walk::Request req = walk.start();
  for (;;) {
    std::string resp;
    sy::Walk::Step step;
    if (req.type == sy::MsgType::TreeReq) {
      CHECK(sy::answer_tree(up, req.payload, resp));
      step = walk.on_tree(down, resp, req);
    } else {
      CHECK(sy::answer_leaves(up, req.payload, resp));
      step = walk.on_leaves(down, resp, keys);
    }
    CHECK(step != sy::Walk::Step::Malformed);
    if (step != sy::Walk::Step::Next) break;
  }
  if (applied != nullptr) *applied = keys;
  return walk.stats();
}

void empty_stores_agree() {
  sy::Store a, b;
  CHECK(a.root() == b.root());
  const auto s = sync(a, b);
  CHECK(s.round_trips == 1);
  CHECK(s.hashes == 1);
}

void root_depends_on_value_and_version() {
  sy::Store a, b, c;
  a.apply("k", at("v", 1));
  b.apply("k", at("w", 1));
  c.apply("k", at("v", 2));
  CHECK(a.root() != sy::Store().root());
  CHECK(a.root() != b.root());
  CHECK(a.root() != c.root());
}

// Same contents reached in a different order must hash the same, or two nodes
// that agree would keep walking forever.
void root_ignores_insertion_order() {
  sy::Store a, b;
  for (int i = 0; i < 100; ++i) a.apply("k" + std::to_string(i), at("v", i));
  for (int i = 99; i >= 0; --i) b.apply("k" + std::to_string(i), at("v", i));
  CHECK(a.root() == b.root());
}

void older_version_never_wins() {
  sy::Store s;
  CHECK(s.apply("k", at("new", 20)));
  CHECK(!s.apply("k", at("old", 10)));
  CHECK(!s.apply("k", at("same", 20)));
  CHECK(s.get("k") && s.get("k")->value == "new");
}

void first_sync_copies_everything() {
  sy::Store up, down;
  for (int i = 0; i < 1000; ++i) up.apply("k" + std::to_string(i), at("v", i));

  const auto s = sync(up, down);
  CHECK(down.size() == 1000);
  CHECK(down.root() == up.root());
  CHECK(s.applied == 1000);
  // Root, three levels below it, then the leaves.
  CHECK(s.round_trips == 1 + sy::Store::kDepth + 1);

  // And having converged, the next walk stops at the root.
  CHECK(sync(up, down).round_trips == 1);
}

// The property the tree exists for. One changed key among a thousand costs one
// branch of hashes and one entry, not the dataset.
void one_change_costs_one_branch() {
  sy::Store up, down;
  for (int i = 0; i < 1000; ++i) up.apply("k" + std::to_string(i), at("v", i));
  sync(up, down);

  up.apply("k500", at("changed", 5000));
  std::vector<std::string> applied;
  const auto s = sync(up, down, &applied);

  CHECK(s.entries <= 2);  // anything else that shares its leaf comes along
  CHECK(applied == std::vector<std::string>{"k500"});
  CHECK(s.hashes == 1 + sy::Store::kFanout * sy::Store::kDepth);
  CHECK(s.bytes < 1000);
  CHECK(down.get("k500") && down.get("k500")->value == "changed");
  CHECK(down.root() == up.root());
}

// What a downstream sees if its upstream restarted empty: it must keep what it
// has rather than mirror the loss.
void downstream_keeps_what_upstream_lost() {
  sy::Store up, down;
  down.apply("k", at("v", 1));
  const auto s = sync(up, down);
  CHECK(s.applied == 0);
  CHECK(down.get("k") != nullptr);
}

void big_diff_spans_several_walks() {
  sy::Store up, down;
  const std::string value(1000, 'x');
  for (int i = 0; i < 1000; ++i) up.apply("k" + std::to_string(i), at(value, i));

  int walks = 0;
  while (down.root() != up.root() && walks < 20) {
    sync(up, down);
    ++walks;
  }
  CHECK(down.root() == up.root());
  CHECK(walks > 1);  // ~1MB of values against a 256KB budget per response
}

void malformed_requests_are_refused() {
  sy::Store s;
  std::string resp;
  CHECK(!sy::answer_tree(s, "", resp));
  // level 9 does not exist
  CHECK(!sy::answer_tree(s, std::string("\x09\0\0\0\0", 5), resp));
  // root has one node, index 1 is out of range
  CHECK(!sy::answer_tree(s, std::string("\x00\0\0\0\x01\0\0\0\x01", 9), resp));
  // claims four billion indices in a nine byte payload
  CHECK(!sy::answer_leaves(s, std::string("\xff\xff\xff\xff\0\0\0\0", 8), resp));
  // trailing junk
  CHECK(!sy::answer_leaves(s, std::string("\0\0\0\0\x01", 5), resp));
}

void response_to_a_different_question_is_malformed() {
  sy::Store up, down;
  up.apply("k", at("v", 1));

  sy::Walk walk;
  walk.start();
  std::string resp;
  // A level 1 answer to a level 0 question.
  sy::Walk::Request other{sy::MsgType::TreeReq,
                          std::string("\x01\0\0\0\x01\0\0\0\x03", 9)};
  CHECK(sy::answer_tree(up, other.payload, resp));
  sy::Walk::Request next;
  CHECK(walk.on_tree(down, resp, next) == sy::Walk::Step::Malformed);
}


sy::Entry tombstone(std::int64_t wall) {
  sy::Entry e{"", {wall, 0}, {wall, 0}};
  e.tombstone = true;
  return e;
}

void a_delete_travels_like_a_write() {
  sy::Store up, down;
  up.apply("k", at("v", 1));
  sync(up, down);
  CHECK(down.live() == 1);

  up.apply("k", tombstone(2));
  std::vector<std::string> applied;
  sync(up, down, &applied);
  CHECK(applied == std::vector<std::string>{"k"});
  CHECK(down.get("k") && down.get("k")->tombstone);
  CHECK(down.live() == 0);
  CHECK(down.root() == up.root());

  // And an older write that turns up late still loses to it.
  CHECK(!down.apply("k", at("late", 1)));
}

// Both sides have to drop the same tombstones, or their trees never agree.
void downstream_collects_what_upstream_collected() {
  sy::Store up, down;
  up.apply("old", tombstone(10));
  up.apply("new", tombstone(30));
  up.horizon() = {0, 100};
  sync(up, down);
  CHECK(down.size() == 2);

  up.horizon() = {20, 200};
  CHECK(up.collect(20) == 1);
  const auto s = sync(up, down);
  CHECK(s.collected == 1);
  CHECK(!s.wiped);
  CHECK(down.get("old") == nullptr && down.get("new") != nullptr);
  CHECK(down.root() == up.root());
  CHECK(down.horizon().collect_before == 20);
}

// The case the horizon exists for. down holds k, misses both the delete and
// the collection while partitioned, and nothing in a normal walk would ever
// tell it k is gone: upstream simply lacks it, and lacking is what an
// upstream that restarted empty looks like too.
void replica_that_missed_a_collection_starts_over() {
  sy::Store up, down;
  up.apply("k", at("v", 5));
  up.apply("other", at("v", 6));
  up.horizon() = {0, 10};
  sync(up, down);
  CHECK(down.horizon().current_as_of == 10);

  up.apply("k", tombstone(30));
  up.horizon() = {40, 100};
  up.collect(40);

  const auto s = sync(up, down);
  CHECK(s.wiped);
  CHECK(down.get("k") == nullptr);
  CHECK(down.get("other") != nullptr);
  CHECK(down.root() == up.root());
  CHECK(down.horizon().current_as_of == 100);
}

void replica_that_kept_up_is_not_wiped() {
  sy::Store up, down;
  up.apply("k", at("v", 5));
  up.horizon() = {0, 50};
  sync(up, down);

  up.horizon() = {40, 100};
  CHECK(!sync(up, down).wiped);
  CHECK(down.horizon().current_as_of == 100);
}

// Extras are what an upstream that restarted empty leaves behind. Until they
// are reconciled this replica is not equal to upstream, so it must not claim
// to be current.
void extras_hold_back_current_as_of() {
  sy::Store up, down;
  down.apply("mine", at("v", 1));
  up.apply("theirs", at("v", 2));
  up.horizon() = {0, 100};
  sync(up, down);
  CHECK(down.get("mine") != nullptr);
  CHECK(down.horizon().current_as_of == 0);
}

void truncated_walk_does_not_count_as_caught_up() {
  sy::Store up, down;
  const std::string value(1000, 'x');
  for (int i = 0; i < 1000; ++i) up.apply("k" + std::to_string(i), at(value, i));
  up.horizon() = {0, 100};

  sync(up, down);
  CHECK(down.root() != up.root());
  CHECK(down.horizon().current_as_of == 0);
  while (down.root() != up.root()) sync(up, down);
  sync(up, down);
  CHECK(down.horizon().current_as_of == 100);
}

}  // namespace

int main() {
  empty_stores_agree();
  root_depends_on_value_and_version();
  root_ignores_insertion_order();
  older_version_never_wins();
  first_sync_copies_everything();
  one_change_costs_one_branch();
  downstream_keeps_what_upstream_lost();
  big_diff_spans_several_walks();
  malformed_requests_are_refused();
  response_to_a_different_question_is_malformed();
  a_delete_travels_like_a_write();
  downstream_collects_what_upstream_collected();
  replica_that_missed_a_collection_starts_over();
  replica_that_kept_up_is_not_wiped();
  extras_hold_back_current_as_of();
  truncated_walk_does_not_count_as_caught_up();

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
