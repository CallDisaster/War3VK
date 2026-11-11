// Development-only CPU/SPIR-V handoff for keys captured from this source revision.
// This local text fixture is NOT a frozen IPC wire format or a game-draw replay.
// No instance, device, shader registration, GPU operation or Game.dll load occurs.
//
// CLI (requires Windows wmain / -municode):
//   warvk-ff-key-compile --input <fixture.txt> --output <new-directory>
// ASCII fixture, whitespace-separated, no BOM or comments:
//   WARVK_FF_KEYS_V1
//   invariantPosition 1
//   forceSampleRateShading 0
//   vsCount 1
//   fsCount 0
//   VS 0 0 0 0 0
//   END
// Each VS has exactly 5 decimal uint32 words, each FS exactly 16. VS records
// precede FS records. Counts are 0..32 each, at least one key is required, and
// duplicates within a stage are rejected. Only whitespace may follow END.
// Output is vs-000.spv / fs-000.spv in input order and one stdout JSON receipt.
// Failures keep partial files for diagnosis. Accept only a complete receipt AND
// exit 0; an output/pipe failure may leave partial bytes but cannot pass the tool.

#include "../../../src/d3d9/d3d9_fixed_function.h"
#include "../../../src/d3d9/d3d9_options.h"
#include "../../../src/util/log/log.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <windows.h>

namespace dxvk {
  // The util library expects one logger owner per final executable. Before any
  // options/compiler call, main disables file logging in THIS PROCESS only.
  Logger Logger::s_instance("ff-key-compile.log");
}

namespace {
  constexpr size_t kMaxFixtureBytes = 16u * 1024u;
  constexpr size_t kMaxTokenBytes = 64u;
  constexpr size_t kMaxKeysPerStage = 32u;
  constexpr size_t kMaxShaderBytes = 8u * 1024u * 1024u;
  constexpr uint32_t kSpirvMagic = 0x07230203u;
  using VSWords = std::array<uint32_t, 5>;
  using FSWords = std::array<uint32_t, 16>;

  // A same-revision native-key check, not permission to memcpy bitfields across
  // arbitrary ABIs. The independent runner must still compare both-width output.
  static_assert(sizeof(dxvk::D3D9FFShaderKeyVS) == 20);
  static_assert(sizeof(dxvk::D3D9FFShaderKeyFS) == 64);
  static_assert(sizeof(dxvk::D3D9FFShaderStage) == 8);
  static_assert(dxvk::caps::TextureStageCount == 8);
  static_assert(sizeof(void*) == 4 || sizeof(void*) == 8);
  static_assert(kMaxShaderBytes <= std::numeric_limits<DWORD>::max());

  void require(bool condition, const char* message) {
    if (!condition)
      throw std::runtime_error(message);
  }

  std::string jsonString(std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (const unsigned char c : value) {
      if (c == '"' || c == '\\') {
        result += '\\';
        result += char(c);
      } else if (c < 0x20) {
        result += "\\u00";
        result += hex[c >> 4];
        result += hex[c & 15];
      } else {
        result += char(c);
      }
    }
    result += '"';
    return result;
  }

  bool isFixtureSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  }

  class FixtureReader {
  public:
    explicit FixtureReader(std::string_view source) : m_source(source) {}

    std::string_view token() {
      skipSpace();
      require(m_offset < m_source.size(), "fixture ended before a required token");
      const size_t start = m_offset;
      while (m_offset < m_source.size() && !isFixtureSpace(m_source[m_offset])) {
        require(m_offset - start < kMaxTokenBytes, "fixture token exceeds limit");
        ++m_offset;
      }
      return m_source.substr(start, m_offset - start);
    }

    void expect(std::string_view expected) {
      require(token() == expected, "unexpected fixture marker or field order");
    }

    uint32_t decimal() {
      const std::string_view text = token();
      uint32_t result = 0;
      for (const char c : text) {
        require(c >= '0' && c <= '9', "only unsigned decimal words are allowed");
        const uint32_t digit = uint32_t(c - '0');
        require(result <= (std::numeric_limits<uint32_t>::max() - digit) / 10u,
          "fixture uint32 overflow");
        result = result * 10u + digit;
      }
      return result;
    }

    bool boolean(std::string_view field) {
      expect(field);
      const std::string_view text = token();
      require(text == "0" || text == "1", "fixture boolean must be exactly 0 or 1");
      return text == "1";
    }

    uint32_t count(std::string_view field) {
      expect(field);
      const uint32_t value = decimal();
      require(value <= kMaxKeysPerStage, "fixture stage count exceeds limit");
      return value;
    }

    void finish() {
      expect("END");
      skipSpace();
      require(m_offset == m_source.size(), "trailing fixture input is forbidden");
    }

  private:
    void skipSpace() {
      while (m_offset < m_source.size() && isFixtureSpace(m_source[m_offset]))
        ++m_offset;
    }

    std::string_view m_source;
    size_t m_offset = 0;
  };

  struct Fixture {
    bool invariantPosition = false;
    bool forceSampleRateShading = false;
    std::vector<VSWords> vertex;
    std::vector<FSWords> fragment;
  };

  template<size_t WordCount>
  std::vector<std::array<uint32_t, WordCount>> readKeys(
      FixtureReader& input, std::string_view tag, uint32_t count) {
    std::vector<std::array<uint32_t, WordCount>> keys;
    keys.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
      input.expect(tag);
      std::array<uint32_t, WordCount> words{};
      for (uint32_t& word : words)
        word = input.decimal();
      require(std::find(keys.begin(), keys.end(), words) == keys.end(),
        "duplicate fixture key within one stage");
      keys.push_back(words);
    }
    return keys;
  }

  Fixture readFixture(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(file.is_open(), "cannot open input fixture");
    std::array<char, kMaxFixtureBytes + 1u> bytes{};
    file.read(bytes.data(), std::streamsize(bytes.size()));
    require(!file.bad(), "fixture read failed");
    const auto length = file.gcount();
    require(length > 0 && size_t(length) <= kMaxFixtureBytes,
      "fixture is empty or exceeds byte limit");
    const std::string_view text(bytes.data(), size_t(length));
    for (const unsigned char c : text)
      require(c >= 0x20u ? c <= 0x7eu : isFixtureSpace(char(c)),
        "fixture must be ASCII without BOM or control bytes");
    FixtureReader input(text);
    input.expect("WARVK_FF_KEYS_V1");
    Fixture fixture;
    fixture.invariantPosition = input.boolean("invariantPosition");
    fixture.forceSampleRateShading = input.boolean("forceSampleRateShading");
    const uint32_t vertexCount = input.count("vsCount");
    const uint32_t fragmentCount = input.count("fsCount");
    require(vertexCount + fragmentCount != 0, "empty key sample is not accepted");
    fixture.vertex = readKeys<5>(input, "VS", vertexCount);
    fixture.fragment = readKeys<16>(input, "FS", fragmentCount);
    input.finish();
    return fixture;
  }

  // dump() writes the original unpatched SPIR-V, unlike getCode()'s optional
  // resource-binding/linkage patching. Cap the output stream independently.
  class BoundedDumpBuffer final : public std::streambuf {
  public:
    const std::vector<char>& bytes() const { return m_bytes; }

  protected:
    std::streamsize xsputn(const char* source, std::streamsize count) override {
      if (count < 0 || uint64_t(count) > kMaxShaderBytes - m_bytes.size())
        return 0;
      m_bytes.insert(m_bytes.end(), source, source + size_t(count));
      return count;
    }

    int_type overflow(int_type value) override {
      if (traits_type::eq_int_type(value, traits_type::eof()))
        return traits_type::not_eof(value);
      if (m_bytes.size() == kMaxShaderBytes)
        return traits_type::eof();
      m_bytes.push_back(traits_type::to_char_type(value));
      return value;
    }

  private:
    std::vector<char> m_bytes;
  };

  class OutputHandle {
  public:
    explicit OutputHandle(const std::filesystem::path& path) {
      m_value = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
      require(m_value != INVALID_HANDLE_VALUE, "cannot exclusively create SPIR-V file");
    }

    ~OutputHandle() {
      if (m_value != INVALID_HANDLE_VALUE)
        CloseHandle(m_value);
    }

    OutputHandle(const OutputHandle&) = delete;
    OutputHandle& operator=(const OutputHandle&) = delete;

    void write(const std::vector<char>& bytes) {
      require(bytes.size() <= kMaxShaderBytes, "SPIR-V output exceeds byte limit");
      DWORD written = 0;
      require(WriteFile(m_value, bytes.data(), DWORD(bytes.size()), &written, nullptr)
          && size_t(written) == bytes.size(), "SPIR-V output write failed");
      require(FlushFileBuffers(m_value) != FALSE, "SPIR-V output flush failed");
      const HANDLE completed = std::exchange(m_value, INVALID_HANDLE_VALUE);
      require(CloseHandle(completed) != FALSE, "SPIR-V output close failed");
    }

  private:
    HANDLE m_value = INVALID_HANDLE_VALUE;
  };

  struct FileReceipt {
    std::string file;
    std::string debugName;
    size_t bytes = 0;
  };

  template<typename Key>
  FileReceipt compileKey(const Key& key, const dxvk::D3D9Options& options,
      const std::filesystem::path& directory, const char* prefix, size_t index) {
    auto shader = dxvk::CompileD3D9FixedFunctionShader(key, &options);
    require(shader != nullptr, "shared compiler returned no shader");
    BoundedDumpBuffer buffer;
    std::ostream stream(&buffer);
    shader->dump(stream);
    require(stream.good(), "SPIR-V dump failed or exceeded byte limit");
    const auto& bytes = buffer.bytes();
    require(bytes.size() >= 5u * sizeof(uint32_t) && bytes.size() % sizeof(uint32_t) == 0,
      "SPIR-V dump has an invalid size");
    uint32_t magic = 0;
    for (size_t byte = 0; byte < sizeof(uint32_t); ++byte)
      magic |= uint32_t(static_cast<unsigned char>(bytes[byte])) << (8u * byte);
    require(magic == kSpirvMagic, "SPIR-V dump has an invalid magic");
    const std::string number = std::to_string(index);
    require(number.size() <= 3u, "output index exceeds naming bound");
    const std::string filename = std::string(prefix) + "-"
      + std::string(3u - number.size(), '0') + number + ".spv";
    FileReceipt result{filename, shader->debugName(), bytes.size()};
    require(result.debugName.size() <= 128u, "shader debug name exceeds receipt bound");
    OutputHandle output(directory / filename);
    output.write(bytes);
    return result;
  }

  void requireNoGameModule() {
    require(GetModuleHandleW(L"Game.dll") == nullptr
        && GetModuleHandleW(L"War3.exe") == nullptr,
      "this CPU-only tool must not run inside a game process");
  }

  void emitReceipt(const Fixture& fixture, const std::vector<FileReceipt>& files) {
    std::cout << "{\"schema\":\"warvk-ff-key-compile/v1\",\"success\":true"
      << ",\"processBits\":" << sizeof(void*) * 8u
      << ",\"vsKeyBytes\":" << sizeof(dxvk::D3D9FFShaderKeyVS)
      << ",\"fsKeyBytes\":" << sizeof(dxvk::D3D9FFShaderKeyFS)
      << ",\"invariantPosition\":" << (fixture.invariantPosition ? "true" : "false")
      << ",\"forceSampleRateShading\":" << (fixture.forceSampleRateShading ? "true" : "false")
      << ",\"vsCount\":" << fixture.vertex.size()
      << ",\"fsCount\":" << fixture.fragment.size()
      << ",\"noGpu\":true,\"gameRender\":false,\"binaryEquivalenceProven\":false,\"files\":[";
    for (size_t index = 0; index < files.size(); ++index) {
      if (index)
        std::cout << ',';
      const auto& file = files[index];
      std::cout << "{\"file\":" << jsonString(file.file)
        << ",\"bytes\":" << file.bytes << ",\"debugName\":" << jsonString(file.debugName) << '}';
    }
    std::cout << "]}" << std::endl;
    require(std::cout.good(), "stdout receipt write failed");
  }
}

int wmain(int argc, wchar_t** argv) {
  try {
    require(argc == 5 && std::wstring_view(argv[1]) == L"--input"
        && std::wstring_view(argv[3]) == L"--output",
      "usage: warvk-ff-key-compile --input <fixture> --output <new-directory>");
    require(argv[2][0] != L'\0' && argv[4][0] != L'\0', "input/output path must not be empty");
    // Process-local only: prevent shared Logger from overwriting a pre-existing
    // log outside the fresh output directory. No user/system environment edit.
    require(SetEnvironmentVariableW(L"DXVK_LOG_PATH", L"none") != FALSE,
      "cannot disable incidental log-file output");
    requireNoGameModule();
    const Fixture fixture = readFixture(std::filesystem::path(argv[2]));
    const std::filesystem::path output = std::filesystem::absolute(argv[4]);
    // Do not recursively create parents, accept an existing directory, remove
    // anything, or retry into a different location after an ownership failure.
    require(CreateDirectoryW(output.c_str(), nullptr) != FALSE,
      "output directory must be new and its parent must already exist");
    const dxvk::Config config;
    dxvk::D3D9Options options(dxvk::Rc<dxvk::DxvkDevice>{}, config);
    options.invariantPosition = fixture.invariantPosition;
    options.forceSampleRateShading = fixture.forceSampleRateShading;
    std::vector<FileReceipt> files;
    files.reserve(fixture.vertex.size() + fixture.fragment.size());
    for (size_t index = 0; index < fixture.vertex.size(); ++index) {
      dxvk::D3D9FFShaderKeyVS key;
      const auto& words = fixture.vertex[index];
      std::copy(words.begin(), words.end(), key.Data.Primitive);
      files.push_back(compileKey(key, options, output, "vs", index));
    }
    for (size_t index = 0; index < fixture.fragment.size(); ++index) {
      dxvk::D3D9FFShaderKeyFS key;
      const auto& words = fixture.fragment[index];
      for (size_t stage = 0; stage < dxvk::caps::TextureStageCount; ++stage)
        for (size_t word = 0; word < 2u; ++word)
          key.Stages[stage].Primitive[word] = words[stage * 2u + word];
      files.push_back(compileKey(key, options, output, "fs", index));
    }
    requireNoGameModule();
    emitReceipt(fixture, files);
    return 0;
  } catch (const dxvk::DxvkError& error) {
    std::cerr << "{\"success\":false,\"noGpu\":true,\"error\":"
      << jsonString(error.message()) << '}' << std::endl;
  } catch (const std::exception& error) {
    std::cerr << "{\"success\":false,\"noGpu\":true,\"error\":"
      << jsonString(error.what()) << '}' << std::endl;
  } catch (...) {
    std::cerr << "{\"success\":false,\"noGpu\":true,\"error\":\"unknown exception\"}"
      << std::endl;
  }
  return 1;
}
