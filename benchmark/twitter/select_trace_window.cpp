#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using u64 = std::uint64_t;
struct Record { u64 id, write; };

int main(int argc, char** argv) {
  try {
    if (argc != 5) throw std::runtime_error("Expected input, output, statistics and maximum keys");
    const u64 limit = std::stoull(argv[4]);
    if (!limit) throw std::runtime_error("Maximum keys must be positive");
    std::ifstream input(argv[1], std::ios::binary);
    std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
    std::array<char, 112> header{};
    if (!input.read(header.data(), header.size()) || std::memcmp(header.data(), "TTRACE01", 8))
      throw std::runtime_error("Invalid input header");
    std::array<u64, 5> source{};
    std::memcpy(source.data(), header.data() + 8, sizeof(source));
    output.write(header.data(), header.size());
    u64 count = 0, keys = 0, reads = 0, updates = 0, checksum = 0;
    bool stopped = false;
    std::array<Record, 65536> buffer;
    for (u64 offset = 0; offset < source[0] && !stopped;) {
      const u64 batch = std::min<u64>(buffer.size(), source[0] - offset);
      if (!input.read(reinterpret_cast<char*>(buffer.data()), batch * sizeof(Record)))
        throw std::runtime_error("Truncated input records");
      u64 retained = 0;
      for (; retained < batch; ++retained) {
        const auto record = buffer[retained];
        if (!record.id || record.id > source[1] || record.write > 1 || record.id > keys + 1)
          throw std::runtime_error("Input must use consecutive first-seen key identities");
        if (record.id > limit) { stopped = true; break; }
        keys = std::max(keys, record.id);
        ++count; reads += !record.write; updates += record.write;
        checksum = (checksum ^ ((record.id << 1) | record.write)) * 1099511628211ULL;
      }
      output.write(reinterpret_cast<const char*>(buffer.data()), retained * sizeof(Record));
      if (!output) throw std::runtime_error("Window write failed");
      offset += batch;
    }
    if (!count || keys != std::min(limit, source[1]))
      throw std::runtime_error("Window key coverage is incomplete");
    const std::array<u64, 5> result{count, keys, reads, updates, checksum};
    output.seekp(8); output.write(reinterpret_cast<const char*>(result.data()), sizeof(result));
    output.close();
    if (!output) throw std::runtime_error("Window finalization failed");
    std::ofstream stats(argv[3]);
    stats << "{\"records\":" << count << ",\"unique_keys\":" << keys
          << ",\"reads\":" << reads << ",\"updates\":" << updates << "}\n";
    if (!stats) throw std::runtime_error("Statistics write failed");
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
