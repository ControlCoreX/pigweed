// Copyright 2022 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.
#pragma once

#include <cstdint>

namespace pw::bluetooth {

// The maximum length of a device name. This value was selected based on the HCI
// and GAP specifications (v5.2, Vol 4, Part E, 7.3.11 and Vol 3, Part C, 12.1).
inline constexpr uint8_t kMaxDeviceNameLength = 248;

}  // namespace pw::bluetooth
