#include "../runtime/em1_rvz.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
int main(int argc, char** argv) {
  if (argc != 3) return 2;
  std::string error;
  auto source = em2::em1::RvzSource::Open(argv[1], error);
  if (!source) { std::cerr << error << '\n'; return 1; }
  std::ifstream reference(argv[2], std::ios::binary);
  if (!reference) return 2;
  const std::vector<uint8_t> expected{std::istreambuf_iterator<char>(reference), {}};
  std::vector<uint8_t> data;
  if (!source->ReadFile("PACKFILES\\DBC_1ST_ZONEC.PAK", data, error) || data != expected) {
    std::cerr << "Original PAK mismatch: " << error << '\n'; return 1;
  }
  for (size_t offset : {size_t(0), size_t(31), size_t(32765), size_t(131071), expected.size() - 4096}) {
    if (!source->ReadFileRange("packfiles/dbc_1st_zonec.pak", offset, 4096, data, error) ||
        data.size() != 4096 || !std::equal(data.begin(), data.end(), expected.begin() + offset)) {
      std::cerr << "Random source range mismatch: " << error << '\n'; return 1;
    }
  }
  const auto preserved = data;
  if (source->ReadFileRange("packfiles/dbc_1st_zonec.pak", expected.size(), 1, data, error) || data != preserved ||
      source->ReadFile("../packfiles/dbc_1st_zonec.pak", data, error) || data != preserved ||
      source->ReadFile("packfiles/absent.pak", data, error) || data != preserved) return 1;
  std::cout << source->DiscId() << ": " << source->Files().size() << " files; " << expected.size()
            << " original PAK bytes matched; random ranges and invalid reads passed; "
            << source->SourceIdentity() << '\n';
}
