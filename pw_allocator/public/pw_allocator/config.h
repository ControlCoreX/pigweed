// Copyright 2024 The Pigweed Authors
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

#ifndef PW_ALLOCATOR_STRICT_VALIDATION
/// Enables additional checks and crashing on check failure.
///
/// Individual allocators have a number of invariants that must hold at before
/// and after each API call. For example, for a given block that is not first or
/// last `block->Next()->Prev() == block` and `block->Prev()->Next() == block`.
///
/// These invariants can be checked at the beginning an end of each API call,
/// but doing so may become expensive. Additionally, it may not always be clear
/// what should be done in a production setting if an invariant fails, e.g.
/// should it crash, log, or something else?
///
/// As a result, these checks and the behavior to crash on failure are only
/// enabled when strict validation is requested. Strict validation is *always*
/// enabled for tests.
#define PW_ALLOCATOR_STRICT_VALIDATION 0
#endif  // PW_ALLOCATOR_STRICT_VALIDATION

#ifndef PW_ALLOCATOR_BLOCK_POISON_INTERVAL
/// Controls how frequently blocks are poisoned on deallocation.
///
/// Blocks may be "poisoned" when deallocated by writing a pattern to their
/// useable memory space. When next allocated, the pattern is checked to ensure
/// it is unmodified, i.e. that nothing has changed the memory while it was
/// free. If the memory has been changed, then a heap-overflow, use-after-free,
/// or other memory corruption bug exists and the program aborts.
///
/// If set to 0, poisoning is disabled. For any other value N, every Nth block
/// is poisoned. This allows consumers to stochiastically sample allocations for
/// memory corruptions while mitigating the performance impact.
#define PW_ALLOCATOR_BLOCK_POISON_INTERVAL 0
#endif  // PW_ALLOCATOR_BLOCK_POISON_INTERVAL

#ifndef PW_ALLOCATOR_SUPPRESS_DEPRECATED_WARNINGS
/// Suppresses warnings about using legacy allocator interfaces.
///
/// This module is undergoing refactoring to improve flexibility and
/// performance. Some portions of the API that are being updated are in use by
/// downstream consumers. These legacy interfaces are preserved for now, but
/// deprecated.
///
/// Initially, this setting defaults to 1 and these interfaces may still be
/// consumed without warning.  At some point, this will default to 0. Downstream
/// projects may still suppress the warning by overriding this configuration,
/// but must be aware that legacy interfaces will eventually be removed.
///
/// See b/376730645 for background and details.
#define PW_ALLOCATOR_SUPPRESS_DEPRECATED_WARNINGS 1
#endif  // PW_ALLOCATOR_SUPPRESS_DEPRECATED_WARNINGS

#if PW_ALLOCATOR_SUPPRESS_DEPRECATED_WARNINGS
#define PW_ALLOCATOR_DEPRECATED
#else
#define PW_ALLOCATOR_DEPRECATED \
  [[deprecated("See b/376730645 for background and workarounds.")]]
#endif  // PW_ALLOCATOR_SUPPRESS_DEPRECATED_WARNINGS
