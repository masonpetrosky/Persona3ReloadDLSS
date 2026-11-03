// SPDX-License-Identifier: MIT

#include "console_variables.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>

namespace p3rdlss {
namespace {

struct Section {
  const uint8_t* data = nullptr;
  size_t size = 0;
};

// The executable's sections that hold the game's code and those that hold read-only data. Denuvo renames them:
// Persona 3 Reload's own code is in ".tls$", not ".text", so they are told apart by their flags. Denuvo's own code
// (the writable ".text") is left out.
struct Image {
  std::vector<Section> code, data;
};

const Image& GameImage() {
  static const Image image = [] {
    Image result;
    auto* const base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const auto* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
      const IMAGE_SECTION_HEADER& s = sections[i];
      const Section section = {base + s.VirtualAddress, s.Misc.VirtualSize};
      if (s.Characteristics & IMAGE_SCN_MEM_WRITE) continue;
      if (s.Characteristics & IMAGE_SCN_MEM_EXECUTE)
        result.code.push_back(section);
      else if (s.Characteristics & IMAGE_SCN_CNT_INITIALIZED_DATA)
        result.data.push_back(section);
    }
    return result;
  }();
  return image;
}

bool Readable(const void* p, size_t bytes) {
  MEMORY_BASIC_INFORMATION info = {};
  if (VirtualQuery(p, &info, sizeof(info)) != sizeof(info) || info.State != MEM_COMMIT) return false;
  if (info.Protect == 0 || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
  return static_cast<const uint8_t*>(p) + bytes <= static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize;
}

// Calls scan(begin, size) for each committed, readable stretch of a section.
template <typename Scan>
void ForReadable(const Section& s, Scan&& scan) {
  const uint8_t* p = s.data;
  const uint8_t* const end = s.data + s.size;
  while (p < end) {
    MEMORY_BASIC_INFORMATION info = {};
    if (VirtualQuery(p, &info, sizeof(info)) != sizeof(info)) return;
    const uint8_t* const region_end =
        (std::min)(end, static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize);
    if (region_end <= p) return;
    if (info.State == MEM_COMMIT && info.Protect != 0 && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
      scan(p, static_cast<size_t>(region_end - p));
    p = region_end;
  }
}

// Where a RIP-relative instruction of `length` bytes, ending with its displacement, points.
const uint8_t* RipTarget(const uint8_t* instruction, size_t length) {
  int32_t disp;
  std::memcpy(&disp, instruction + length - 4, 4);
  return instruction + length + disp;
}

// The code that loads the exact name into rdx (lea rdx, [rip+name]), as code that registers or looks up a console
// variable does.
std::vector<const uint8_t*> NameLoads(const wchar_t* name) {
  const Image& image = GameImage();
  std::vector<const uint8_t*> loads;
  const size_t name_bytes = (std::wcslen(name) + 1) * sizeof(wchar_t);  // with the terminator: the exact name only
  std::vector<const uint8_t*> strings;
  for (const Section& s : image.data)
    ForReadable(s, [&](const uint8_t* data, size_t size) {
      for (size_t i = 0; i + name_bytes <= size; i += 2)
        if (std::memcmp(data + i, name, name_bytes) == 0) strings.push_back(data + i);
    });
  if (strings.empty()) return loads;
  for (const Section& s : image.code)
    ForReadable(s, [&](const uint8_t* data, size_t size) {
      for (size_t i = 0; i + 7 <= size; ++i) {
        const uint8_t* p = data + i;
        if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x15) {
          const uint8_t* target = RipTarget(p, 7);
          for (const uint8_t* str : strings)
            if (target == str) loads.push_back(p);
        }
      }
    });
  return loads;
}

// UE 4.27 inlines TAutoConsoleVariable<T>'s constructor: the console manager's RegisterConsoleVariable (name in rdx)
// returns the variable in rax, which goes to the static object's Target (mov [rip+X], rax); its AsVariableInt() or
// AsVariableFloat() result, the TConsoleVariableData, goes to the next field, Ref (mov [rip+X+8], rax), a few
// instructions later. The data starts with the two copies of the value (4 bytes each for int32 and float).
void* FindConsoleVariableData(const wchar_t* name, std::string* detail) {
  const std::vector<const uint8_t*> loads = NameLoads(name);
  if (loads.empty()) {
    if (detail != nullptr) *detail = "no code loads the name";
    return nullptr;
  }
  for (const uint8_t* load : loads) {
    std::vector<const uint8_t*> stores;  // the slots written with mov [rip+X], rax
    for (size_t at = 7; at + 7 <= 96 && Readable(load + at, 7); ++at) {
      const uint8_t* p = load + at;
      if (p[0] == 0x48 && p[1] == 0x89 && p[2] == 0x05) stores.push_back(RipTarget(p, 7));
    }
    for (size_t i = 0; i + 1 < stores.size(); ++i) {
      if (stores[i + 1] != stores[i] + 8 || !Readable(stores[i + 1], sizeof(void*))) continue;
      void* const values = *reinterpret_cast<void* const*>(stores[i + 1]);
      if (values == nullptr || !Readable(values, 2 * sizeof(int32_t))) continue;
      if (detail != nullptr) {
        char text[96];
        std::snprintf(text, sizeof(text), "registered at %p, values at %p", static_cast<const void*>(load), values);
        *detail = text;
      }
      return values;
    }
  }
  if (detail != nullptr) *detail = std::to_string(loads.size()) + " name load(s), none in the expected pattern";
  return nullptr;
}

}  // namespace

int32_t* FindIntConsoleVariable(const wchar_t* name, std::string* detail) {
  return static_cast<int32_t*>(FindConsoleVariableData(name, detail));
}

float* FindFloatConsoleVariable(const wchar_t* name, std::string* detail) {
  return static_cast<float*>(FindConsoleVariableData(name, detail));
}

}  // namespace p3rdlss
