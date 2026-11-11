#include "../runtime/em1_scene_adapter.h"
#include <fstream>
#include <iostream>
#include <iterator>
std::vector<uint8_t> Read(const char* path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("Cannot open fixture");
  return {std::istreambuf_iterator<char>(file), {}};
}
int main(int argc, char** argv) {
  if (argc != 3) return 2;
  const auto source = Read(argv[1]), expected = Read(argv[2]);
  em2::em1::SceneAdapterResult result;
  std::string error;
  if (!em2::em1::AdaptScene(source, result, error) || result.bytes != expected) {
    std::cerr << "Native scene mismatch: " << error << '\n'; return 1;
  }
  const auto valid = result.bytes;
  for (size_t length : {size_t(0), size_t(4), size_t(27), source.size() - 1}) {
    if (em2::em1::AdaptScene(std::span(source).first(length), result, error) || result.bytes != valid) {
      std::cerr << "Truncated source accepted or failure changed output\n"; return 1;
    }
  }
  auto corrupt = source;
  std::fill_n(corrupt.begin(), 4, uint8_t(255));
  if (em2::em1::AdaptScene(corrupt, result, error) || result.bytes != valid) return 1;
  std::cout << result.entity_count << " entities; exact native/oracle bytes and malformed-input checks passed\n";
}
