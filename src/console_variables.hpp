// Finds Unreal Engine console variables in the game's executable from the code that registers them, so the add-on
// can read and write their values. Adapted from FF7RebirthNpcMotion's FindConsoleVariable.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string>

namespace p3rdlss {

// The values of an int console variable registered through a TAutoConsoleVariable<int32> (TConsoleVariableData:
// [0] the game thread's copy, [1] the render thread's), or null. `detail` receives how it was found or why not.
// Call after the executable's static initializers have run (from the device creation on).
int32_t* FindIntConsoleVariable(const wchar_t* name, std::string* detail);
// The same for a TAutoConsoleVariable<float>, whose registration the compiler lays out the same way.
float* FindFloatConsoleVariable(const wchar_t* name, std::string* detail);

}  // namespace p3rdlss
