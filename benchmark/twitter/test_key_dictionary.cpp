#include "compact_key_dictionary.hpp"
#include <iostream>
#include <string>
#include <unordered_map>
void Require(bool value) { if (!value) throw std::runtime_error("Dictionary validation failed"); }
struct ConstantHash { std::size_t operator()(std::string_view) const { return 17; } };
int main() {
  ExactKeyDictionary<ConstantHash> collisions;
  for (unsigned i = 0; i < 1500; ++i) Require(collisions.Intern("collision-" + std::to_string(i)) == i + 1);
  for (unsigned i = 0; i < 1500; ++i) Require(collisions.Find("collision-" + std::to_string(i)) == i + 1);
  Require(collisions.Find("absent") == 0);
  ExactKeyDictionary<> dictionary;
  constexpr unsigned count = (1U << 20) + 177;
  for (unsigned i = 0; i < count; ++i) {
    const std::string key = "key-" + std::to_string(i) + std::string(i % 100, 'x');
    Require(dictionary.Intern(key) == i + 1);
    Require(dictionary.Intern(key) == i + 1);
  }
  for (unsigned i = 0; i < count; ++i)
    Require(dictionary.Find("key-" + std::to_string(i) + std::string(i % 100, 'x')) == i + 1);
  const std::string binary("a\0b,\xff", 5);
  const auto id = dictionary.Intern(binary);
  Require(dictionary.Find(binary) == id && dictionary.Find("a") == 0);
  const std::string huge((1U << 24) - 4, 'z');
  const auto huge_id = dictionary.Intern(huge);
  Require(dictionary.Find(huge) == huge_id);
  bool rejected = false;
  try { dictionary.Intern(huge + "z"); } catch (const std::runtime_error&) { rejected = true; }
  Require(rejected);
  std::cout << "PASS: exact collisions, stable IDs, growth, arena and offset boundaries, binary keys and size limits\n";
}
