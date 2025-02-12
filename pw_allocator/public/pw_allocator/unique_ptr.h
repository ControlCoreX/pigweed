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

#include <cstddef>
#include <memory>
#include <utility>

#include "pw_allocator/capability.h"
#include "pw_preprocessor/compiler.h"

namespace pw {

namespace uniqueptr::internal {
struct Empty {};
}  // namespace uniqueptr::internal

// Forward declaration.
class Deallocator;

namespace allocator::internal {

/// This class simply provides type-erased static methods to check capabilities
/// and deallocate memory in a unique pointer. This allows ``UniquePtr<T>`` to
/// be declared without a complete declaration of ``Deallocator``, breaking the
/// dependency cycle between ``UniquePtr<T>` and ``Allocator::MakeUnique<T>()``.
class BaseUniquePtr {
 protected:
  using Capability = ::pw::allocator::Capability;

  static bool HasCapability(Deallocator* deallocator, Capability capability);
  static void Deallocate(Deallocator* deallocator, void* ptr);
};

}  // namespace allocator::internal

/// An RAII pointer to a value of type ``T`` stored in memory provided by a
/// ``Deallocator``.
///
/// This is analogous to ``std::unique_ptr``, but includes a few differences
/// in order to support ``Deallocator`` and encourage safe usage. Most
/// notably, ``UniquePtr<T>`` cannot be constructed from a ``T*``.
template <typename T>
class UniquePtr : public allocator::internal::BaseUniquePtr {
 public:
  using UnderlyingType =
      std::conditional_t<std::is_array_v<T>,
                         typename std::remove_extent<T>::type,
                         T>;
  using Base = ::pw::allocator::internal::BaseUniquePtr;

  /// Creates an empty (``nullptr``) instance.
  ///
  /// NOTE: Instances of this type are most commonly constructed using
  /// ``Deallocator::MakeUnique``.
  constexpr UniquePtr() : value_(nullptr), deallocator_(nullptr) {
    if constexpr (std::is_array_v<T>) {
      size_ = 0;
    }
  }

  /// Creates an empty (``nullptr``) instance.
  ///
  /// NOTE: Instances of this type are most commonly constructed using
  /// ``Deallocator::MakeUnique``.
  constexpr UniquePtr(std::nullptr_t) : UniquePtr() {}

  /// Move-constructs a ``UniquePtr<T>`` from a ``UniquePtr<U>``.
  ///
  /// This allows not only pure move construction where ``T == U``, but also
  /// converting construction where ``T`` is a base class of ``U``, like
  /// ``UniquePtr<Base> base(deallocator.MakeUnique<Child>());``.
  template <typename U>
  UniquePtr(UniquePtr<U>&& other) noexcept
      : value_(other.value_),
        deallocator_(other.deallocator_),
        size_(other.size_) {
    static_assert(
        std::is_assignable_v<UnderlyingType*&,
                             typename UniquePtr<U>::UnderlyingType*>,
        "Attempted to construct a UniquePtr<T> from a UniquePtr<U> where "
        "U* is not assignable to T*.");
    other.Release();
  }

  // Move-only. These are needed since the templated move-contructor and
  // move-assignment operator do not exactly match the signature of the default
  // move-contructor and move-assignment operator, and thus do not implicitly
  // delete the copy-contructor and copy-assignment operator.
  UniquePtr(const UniquePtr&) = delete;
  UniquePtr& operator=(const UniquePtr&) = delete;

  /// Move-assigns a ``UniquePtr<T>`` from a ``UniquePtr<U>``.
  ///
  /// This operation destructs and deallocates any value currently stored in
  /// ``this``.
  ///
  /// This allows not only pure move assignment where ``T == U``, but also
  /// converting assignment where ``T`` is a base class of ``U``, like
  /// ``UniquePtr<Base> base = deallocator.MakeUnique<Child>();``.
  template <typename U>
  UniquePtr& operator=(UniquePtr<U>&& other) noexcept {
    static_assert(std::is_assignable_v<UnderlyingType*&,
                                       typename UniquePtr<U>::UnderlyingType*>,
                  "Attempted to assign a UniquePtr<U> to a UniquePtr<T> where "
                  "U* is not assignable to T*.");
    Reset();
    value_ = other.value_;
    deallocator_ = other.deallocator_;
    size_ = other.size_;
    other.Release();
    return *this;
  }

  /// Sets this ``UniquePtr`` to null, destructing and deallocating any
  /// currently-held value.
  ///
  /// After this function returns, this ``UniquePtr`` will be in an "empty"
  /// (``nullptr``) state until a new value is assigned.
  UniquePtr& operator=(std::nullptr_t) {
    Reset();
    return *this;
  }

  /// Destructs and deallocates any currently-held value.
  ~UniquePtr() { Reset(); }

  /// Returns a pointer to the object that can destroy the value.
  Deallocator* deallocator() const { return deallocator_; }

  /// Releases a value from the ``UniquePtr`` without destructing or
  /// deallocating it.
  ///
  /// After this call, the object will have an "empty" (``nullptr``) value.
  UnderlyingType* Release() {
    UnderlyingType* value = value_;
    value_ = nullptr;
    deallocator_ = nullptr;
    if constexpr (std::is_array_v<T>) {
      size_ = 0;
    }
    return value;
  }

  /// Destructs and deallocates any currently-held value.
  ///
  /// After this function returns, this ``UniquePtr`` will be in an "empty"
  /// (``nullptr``) state until a new value is assigned.
  void Reset() {
    if (value_ == nullptr) {
      return;
    }
    if (!Base::HasCapability(deallocator_, Capability::kSkipsDestroy)) {
      if constexpr (std::is_array_v<T>) {
        for (size_t i = 0; i < size_; ++i) {
          std::destroy_at(value_ + i);
        }
      } else {
        std::destroy_at(value_);
      }
    }
    Base::Deallocate(deallocator_, value_);
    Release();
  }

  /// ``operator bool`` is not provided in order to ensure that there is no
  /// confusion surrounding ``if (foo)`` vs. ``if (*foo)``.
  ///
  /// ``nullptr`` checking should instead use ``if (foo == nullptr)``.
  explicit operator bool() const = delete;

  /// Returns whether this ``UniquePtr`` is in an "empty" (``nullptr``) state.
  bool operator==(std::nullptr_t) const { return value_ == nullptr; }

  /// Returns whether this ``UniquePtr`` is not in an "empty" (``nullptr``)
  /// state.
  bool operator!=(std::nullptr_t) const { return value_ != nullptr; }

  /// Returns the underlying (possibly null) pointer.
  UnderlyingType* get() { return value_; }
  /// Returns the underlying (possibly null) pointer.
  const UnderlyingType* get() const { return value_; }

  /// Permits accesses to members of ``T`` via ``my_unique_ptr->Member``.
  ///
  /// The behavior of this operation is undefined if this ``UniquePtr`` is in an
  /// "empty" (``nullptr``) state.
  UnderlyingType* operator->() { return value_; }
  const UnderlyingType* operator->() const { return value_; }

  /// Returns a reference to any underlying value.
  ///
  /// The behavior of this operation is undefined if this ``UniquePtr`` is in an
  /// "empty" (``nullptr``) state.
  UnderlyingType& operator*() { return *value_; }
  const UnderlyingType& operator*() const { return *value_; }

  /// Returns a reference to the element at the given index.
  ///
  /// The behavior of this operation is undefined if this ``UniquePtr`` is in an
  /// "empty" (``nullptr``) state.
  template <typename U = T,
            typename = std::enable_if_t<std::is_array_v<U>, bool>>
  UnderlyingType& operator[](size_t index) {
    return value_[index];
  }

  template <typename U = T,
            typename = std::enable_if_t<std::is_array_v<U>, bool>>
  const UnderlyingType& operator[](size_t index) const {
    return value_[index];
  }

  /// Returns the number of elements allocated.
  ///
  /// This will assert if it is called on a non-array type UniquePtr.
  template <typename U = T,
            typename = std::enable_if_t<std::is_array_v<U>, bool>>
  size_t size() const {
    return size_;
  }

 private:
  /// A pointer to the contained value.
  UnderlyingType* value_;

  /// The ``deallocator_`` which provided the memory for  ``value_``.
  /// This must be tracked in order to deallocate the memory upon destruction.
  Deallocator* deallocator_;

  /// The number of elements allocated. This will not be present in the case
  /// where T is not an array type as this will be the empty struct type
  /// optimized out.
  PW_NO_UNIQUE_ADDRESS
  std::conditional_t<std::is_array_v<T>, size_t, uniqueptr::internal::Empty>
      size_;

  /// Allow converting move constructor and assignment to access fields of
  /// this class.
  ///
  /// Without this, ``UniquePtr<U>`` would not be able to access fields of
  /// ``UniquePtr<T>``.
  template <typename U>
  friend class UniquePtr;

  class PrivateConstructorType {};
  static constexpr PrivateConstructorType kPrivateConstructor{};

 public:
  /// Private constructor that is public only for use with `emplace` and
  /// other in-place construction functions.
  ///
  /// Constructs a ``UniquePtr`` from an already-allocated value.
  ///
  /// NOTE: Instances of this type are most commonly constructed using
  /// ``Deallocator::MakeUnique``.
  UniquePtr(PrivateConstructorType,
            UnderlyingType* value,
            Deallocator* deallocator)
      : value_(value), deallocator_(deallocator) {}

  /// Private constructor that is public only for use with `emplace` and
  /// other in-place construction functions.
  ///
  /// Constructs a ``UniquePtr`` from an already-allocated value and size.
  ///
  /// NOTE: Instances of this type are most commonly constructed using
  /// ``Deallocator::MakeUnique``.
  UniquePtr(PrivateConstructorType,
            UnderlyingType* value,
            Deallocator* deallocator,
            size_t size)
      : value_(value), deallocator_(deallocator), size_(size) {}

  // Allow construction with ``kPrivateConstructor`` to the implementation
  // of ``MakeUnique``.
  friend class Deallocator;
};

namespace allocator {

// Alias for module consumers using the older name for the above type.
template <typename T>
using UniquePtr = ::pw::UniquePtr<T>;

}  // namespace allocator
}  // namespace pw
