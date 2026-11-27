#include "outbox.hpp"

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

void drains_in_version_order() {
  sy::Outbox o(10);
  o.add("b", at("2", 20));
  o.add("c", at("3", 30));
  o.add("a", at("1", 10));
  const auto d = o.due(0, 0);
  CHECK(d.size() == 3);
  CHECK(d.size() == 3 && d[0].first == "a" && d[1].first == "b" &&
        d[2].first == "c");
}

void coalesces_by_key() {
  sy::Outbox o(10);
  for (int i = 1; i <= 100; ++i) o.add("k", at(std::to_string(i), i));
  CHECK(o.size() == 1);
  const auto d = o.due(0, 0);
  CHECK(d.size() == 1 && d[0].second.value == "100");

  // A late, older write to the same key changes nothing.
  o.add("k", at("stale", 5));
  CHECK(o.due(0, 0)[0].second.value == "100");
}

void full_refuses_new_keys_but_not_updates() {
  sy::Outbox o(2);
  CHECK(o.add("a", at("1", 1)));
  CHECK(o.add("b", at("1", 2)));
  CHECK(!o.add("c", at("1", 3)));
  CHECK(o.add("a", at("2", 4)));
  CHECK(o.size() == 2);
}

void settles_on_same_or_newer_version() {
  sy::Outbox o(10);
  o.add("mine", at("v", 10));
  o.add("overwritten", at("v", 10));
  o.add("not-back-yet", at("v", 10));
  o.add("older-there", at("v", 10));

  sy::Store s;
  s.apply("mine", at("v", 10));
  s.apply("overwritten", at("someone else", 50));
  s.apply("older-there", at("v", 5));

  const auto landed = o.settle(s);
  CHECK(landed.size() == 2);
  CHECK(o.size() == 2);
  const auto left = o.due(0, 0);
  CHECK(left.size() == 2 && left[0].first == "not-back-yet" &&
        left[1].first == "older-there");
}

void resends_only_what_has_waited() {
  sy::Outbox o(10);
  o.add("a", at("1", 1));
  CHECK(o.due(100, 50).size() == 1);
  CHECK(o.due(120, 50).empty());
  o.add("b", at("1", 2));
  const auto d = o.due(130, 50);
  CHECK(d.size() == 1 && d[0].first == "b");
  CHECK(o.due(150, 50).size() == 1);  // a, 50 after it was last sent
  // A reconnect wants everything, regardless.
  CHECK(o.due(151, 0).size() == 2);
}

// A newer write to a key that was already sent has to go out again, not wait
// out the interval.
void update_is_due_immediately() {
  sy::Outbox o(10);
  o.add("a", at("1", 1));
  o.due(100, 50);
  o.add("a", at("2", 2));
  const auto d = o.due(101, 50);
  CHECK(d.size() == 1 && d[0].second.value == "2");
}

void expires_what_source_would_refuse() {
  sy::Outbox o(10);
  o.add("old", at("v", 10));
  o.add("new", at("v", 30));
  CHECK(o.expire(20) == std::vector<std::string>{"old"});
  CHECK(o.size() == 1);
}

}  // namespace

int main() {
  drains_in_version_order();
  coalesces_by_key();
  full_refuses_new_keys_but_not_updates();
  settles_on_same_or_newer_version();
  resends_only_what_has_waited();
  update_is_due_immediately();
  expires_what_source_would_refuse();

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
