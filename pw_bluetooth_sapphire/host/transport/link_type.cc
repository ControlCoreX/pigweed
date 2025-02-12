// Copyright 2023 The Pigweed Authors
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

#include "pw_bluetooth_sapphire/internal/host/transport/link_type.h"

#include <pw_assert/check.h>

namespace bt {

std::string LinkTypeToString(LinkType type) {
  switch (type) {
    case LinkType::kACL:
      return "ACL";
    case LinkType::kSCO:
      return "SCO";
    case LinkType::kESCO:
      return "ESCO";
    case LinkType::kLE:
      return "LE";
  }

  PW_CRASH("invalid link type: %u", static_cast<unsigned int>(type));
  return "(invalid)";
}

}  // namespace bt
