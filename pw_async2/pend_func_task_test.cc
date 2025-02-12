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

#include "pw_async2/pend_func_task.h"

#include "pw_async2/dispatcher.h"
#include "pw_function/function.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::Function;
using ::pw::async2::Context;
using ::pw::async2::Dispatcher;
using ::pw::async2::PendFuncTask;
using ::pw::async2::Pending;
using ::pw::async2::Poll;
using ::pw::async2::Ready;
using ::pw::async2::Waker;

TEST(PendFuncTask, PendDelegatesToFunc) {
  Dispatcher dispatcher;

  Waker waker;
  int poll_count = 0;
  bool allow_completion = false;

  PendFuncTask func_task([&](Context& cx) -> Poll<> {
    ++poll_count;
    if (allow_completion) {
      return Ready();
    }
    PW_ASYNC_STORE_WAKER(cx, waker, "func_task is waiting for waker");
    return Pending();
  });

  dispatcher.Post(func_task);

  EXPECT_EQ(poll_count, 0);
  EXPECT_EQ(dispatcher.RunUntilStalled(), Pending());
  EXPECT_EQ(poll_count, 1);

  // Unwoken task is not polled.
  EXPECT_EQ(dispatcher.RunUntilStalled(), Pending());
  EXPECT_EQ(poll_count, 1);

  std::move(waker).Wake();
  allow_completion = true;
  EXPECT_EQ(dispatcher.RunUntilStalled(), Ready());
  EXPECT_EQ(poll_count, 2);
}

TEST(PendFuncTask, HoldsCallableByDefault) {
  auto callable = [](Context&) -> Poll<> { return Ready(); };
  PendFuncTask func_task(std::move(callable));
  static_assert(std::is_same<decltype(func_task),
                             PendFuncTask<decltype(callable)>>::value);
}

TEST(PendFuncTask, HoldsPwFunctionWithEmptyTypeList) {
  PendFuncTask<> func_task([](Context&) -> Poll<> { return Ready(); });
  static_assert(std::is_same<decltype(func_task),
                             PendFuncTask<Function<Poll<>(Context&)>>>::value);
}

}  // namespace
