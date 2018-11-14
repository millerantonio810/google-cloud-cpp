// Copyright 2018 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "google/cloud/bigtable/internal/async_poll_op.h"
#include "google/cloud/bigtable/table.h"
#include "google/cloud/bigtable/testing/internal_table_test_fixture.h"
#include "google/cloud/bigtable/testing/mock_completion_queue.h"
#include "google/cloud/bigtable/testing/mock_sample_row_keys_reader.h"
#include "google/cloud/bigtable/testing/table_test_fixture.h"
#include "google/cloud/testing_util/chrono_literals.h"
#include <future>
#include <thread>
#include <typeinfo>

namespace google {
namespace cloud {
namespace bigtable {
inline namespace BIGTABLE_CLIENT_NS {
namespace noex {
namespace {

namespace bt = ::google::cloud::bigtable;
namespace btproto = google::bigtable::v2;
using namespace google::cloud::testing_util::chrono_literals;
using namespace ::testing;

class NoexTableAsyncPollOpTest
    : public bigtable::testing::internal::TableTestFixture {};

using Functor = std::function<void(CompletionQueue&, bool, grpc::Status&)>;

// Operation passed to AsyncPollOp needs to be movable or copyable. Mocked
// objects are neither, so we'll mock this class and hold a shared_ptr to it in
// DummyOperation. DummyOperation will satisfy the requirements for a parameter
// to AsyncPollOp.
// Also, gmock doesn't support rvalue references and templates, so workaround
// that too.
class DummyOperationImpl {
 public:
  virtual ~DummyOperationImpl() = default;
  virtual std::shared_ptr<AsyncOperation> Start(
      CompletionQueue&, std::unique_ptr<grpc::ClientContext>&,
      Functor const&) = 0;
  virtual int AccumulatedResult() = 0;
};

class DummyOperation {
 public:
  using Response = int;

  DummyOperation(std::shared_ptr<DummyOperationImpl> impl) : impl_(impl) {}

  template <typename F,
            typename std::enable_if<
                google::cloud::internal::is_invocable<F, CompletionQueue&, bool,
                                                      grpc::Status&>::value,
                int>::type valid_callback_type = 0>
  std::shared_ptr<AsyncOperation> Start(
      CompletionQueue& cq, std::unique_ptr<grpc::ClientContext>&& context,
      F&& callback) {
    std::unique_ptr<grpc::ClientContext> context_moved(std::move(context));
    return impl_->Start(cq, context_moved, std::move(callback));
  }

  virtual int AccumulatedResult() { return impl_->AccumulatedResult(); }

 private:
  std::shared_ptr<DummyOperationImpl> impl_;
};

class AsyncOperationMock : public AsyncOperation {
 public:
  bool Notify(CompletionQueue& cq, bool ok) override {
    // TODO(#1389) Notify should be moved from AsyncOperation to some more
    // specific derived class.
    google::cloud::internal::RaiseLogicError(
        "This member function doesn't make sense in "
        "AsyncPollOp");
  }

  void Cancel() override {
    google::cloud::internal::RaiseLogicError(
        "Cancellation is not yet supported in AsyncPollOp");
  }
};

class DummyOperationMock : public DummyOperationImpl {
 public:
  MOCK_METHOD3(Start,
               std::shared_ptr<AsyncOperation>(
                   CompletionQueue&, std::unique_ptr<grpc::ClientContext>&,
                   Functor const&));
  MOCK_METHOD0(AccumulatedResult, int());
};

class ImmediateFinishTestConfig {
 public:
  // Error code returned from the first Start()
  grpc::StatusCode error_code;
  // finished flag returned from the second Start()
  bool finished;
  bool exhausted;
  grpc::StatusCode expected;
};

class NoexTableAsyncPollOpImmediateFinishTest
    : public bigtable::testing::internal::TableTestFixture,
      public WithParamInterface<ImmediateFinishTestConfig> {};

TEST_P(NoexTableAsyncPollOpImmediateFinishTest, ImmediateFinish) {
  auto const config = GetParam();
  internal::RPCPolicyParameters const noRetries = {
      std::chrono::hours(0),
      std::chrono::hours(0),
      std::chrono::hours(0),
  };
  auto polling_policy = bigtable::DefaultPollingPolicy(
      config.exhausted ? noRetries : internal::kBigtableLimits);
  MetadataUpdatePolicy metadata_update_policy(kTableId,
                                              MetadataParamTypes::TABLE_NAME);

  auto cq_impl = std::make_shared<bigtable::testing::MockCompletionQueue>();
  CompletionQueue cq(cq_impl);

  auto dummy_op_mock = std::make_shared<DummyOperationMock>();
  auto async_op_mock = new AsyncOperationMock;
  auto async_op_mock_smart = std::shared_ptr<AsyncOperation>(async_op_mock);

  Functor stored_callback;
  bool op_completed = false;

  auto callback = [&op_completed, config](CompletionQueue&, int& response,
                                          grpc::Status& status) {
    EXPECT_EQ(config.expected, status.error_code());
    EXPECT_EQ(27, response);
    op_completed = true;
  };

  auto async_op = std::make_shared<
      internal::AsyncPollOp<decltype(callback), DummyOperation>>(
      __func__, polling_policy->clone(), metadata_update_policy,
      std::move(callback), DummyOperation(dummy_op_mock));

  EXPECT_CALL(*dummy_op_mock, Start(_, _, _))
      .WillOnce(
          Invoke([&stored_callback, &async_op_mock_smart](
                     CompletionQueue&, std::unique_ptr<grpc::ClientContext>&,
                     Functor const& callback) {
            stored_callback = callback;
            return async_op_mock_smart;
          }));
  EXPECT_CALL(*dummy_op_mock, AccumulatedResult()).WillOnce(Invoke([]() {
    return 27;
  }));

  EXPECT_FALSE(stored_callback);
  async_op->Start(cq);
  EXPECT_TRUE(stored_callback);

  grpc::Status status(config.error_code, "");
  stored_callback(cq, config.finished, status);

  EXPECT_TRUE(cq_impl->empty());
  EXPECT_TRUE(op_completed);
}

INSTANTIATE_TEST_CASE_P(
    ImmediateFinish, NoexTableAsyncPollOpImmediateFinishTest,
    ::testing::Values(
        // Finished in first shot.
        ImmediateFinishTestConfig{grpc::StatusCode::OK, true, false,
                                  grpc::StatusCode::OK},
        // Finished in first shot, policy exhausted.
        ImmediateFinishTestConfig{grpc::StatusCode::OK, true, true,
                                  grpc::StatusCode::OK},
        // Finished in first shot but returned UNAVAILABLE
        ImmediateFinishTestConfig{grpc::StatusCode::UNAVAILABLE, true, false,
                                  grpc::StatusCode::UNAVAILABLE},
        // Finished in first shot but returned PERMISSION_DENIED
        ImmediateFinishTestConfig{grpc::StatusCode::PERMISSION_DENIED, true,
                                  false, grpc::StatusCode::PERMISSION_DENIED},
        // RPC succeeded, operation not finished, policy exhausted
        ImmediateFinishTestConfig{grpc::StatusCode::OK, false, true,
                                  grpc::StatusCode::UNKNOWN},
        // RPC failed with UNAVAILABLE, policy exhausted
        ImmediateFinishTestConfig{grpc::StatusCode::UNAVAILABLE, false, true,
                                  grpc::StatusCode::UNAVAILABLE},
        // RPC failed with PERMISSION_DENIED, policy exhausted
        ImmediateFinishTestConfig{grpc::StatusCode::PERMISSION_DENIED, false,
                                  true, grpc::StatusCode::PERMISSION_DENIED}));

class OneRetryTestConfig {
 public:
  // Error code returned from the first Start()
  grpc::StatusCode error_code1;
  // finished flag returned from the second Start()
  bool finished1;
  // Error code returned from the second Start()
  grpc::StatusCode error_code2;
  // finished flag returned from the second Start()
  bool finished2;
  grpc::StatusCode expected;
};

class NoexTableAsyncPollOpOneRetryTest
    : public bigtable::testing::internal::TableTestFixture,
      public WithParamInterface<OneRetryTestConfig> {};

TEST_P(NoexTableAsyncPollOpOneRetryTest, OneRetry) {
  auto const config = GetParam();
  auto polling_policy =
      bigtable::DefaultPollingPolicy(internal::kBigtableLimits);
  MetadataUpdatePolicy metadata_update_policy(kTableId,
                                              MetadataParamTypes::TABLE_NAME);

  auto cq_impl = std::make_shared<bigtable::testing::MockCompletionQueue>();
  CompletionQueue cq(cq_impl);

  auto dummy_op_mock = std::make_shared<DummyOperationMock>();
  auto async_op_mock = new AsyncOperationMock;
  auto async_op_mock_smart = std::shared_ptr<AsyncOperation>(async_op_mock);

  Functor stored_callback1;
  Functor stored_callback2;
  bool op_completed = false;

  auto callback = [&op_completed, config](CompletionQueue&, int& response,
                                          grpc::Status& status) {
    EXPECT_EQ(config.expected, status.error_code());
    EXPECT_EQ(27, response);
    op_completed = true;
  };

  auto async_op = std::make_shared<
      internal::AsyncPollOp<decltype(callback), DummyOperation>>(
      __func__, polling_policy->clone(), metadata_update_policy,
      std::move(callback), DummyOperation(dummy_op_mock));

  EXPECT_CALL(*dummy_op_mock, Start(_, _, _))
      .WillOnce(
          Invoke([&stored_callback1, &async_op_mock_smart](
                     CompletionQueue&, std::unique_ptr<grpc::ClientContext>&,
                     Functor const& callback) {
            stored_callback1 = callback;
            return async_op_mock_smart;
          }))
      .WillOnce(
          Invoke([&stored_callback2, &async_op_mock_smart](
                     CompletionQueue&, std::unique_ptr<grpc::ClientContext>&,
                     Functor const& callback) {
            stored_callback2 = callback;
            return async_op_mock_smart;
          }));
  EXPECT_CALL(*dummy_op_mock, AccumulatedResult()).WillOnce(Invoke([]() {
    return 27;
  }));

  EXPECT_FALSE(stored_callback1);
  async_op->Start(cq);
  EXPECT_TRUE(stored_callback1);

  {
    grpc::Status status(config.error_code1, "");
    stored_callback1(cq, config.finished1, status);
  }

  EXPECT_EQ(1U, cq_impl->size());  // It's the timer
  EXPECT_FALSE(op_completed);

  cq_impl->SimulateCompletion(cq, true);

  EXPECT_TRUE(cq_impl->empty());
  EXPECT_TRUE(stored_callback2);
  EXPECT_FALSE(op_completed);

  {
    grpc::Status status(config.error_code2, "");
    stored_callback2(cq, config.finished2, status);
  }

  EXPECT_TRUE(cq_impl->empty());
  EXPECT_TRUE(op_completed);
}

INSTANTIATE_TEST_CASE_P(
    OneRetry, NoexTableAsyncPollOpOneRetryTest,
    ::testing::Values(
        // No error, not finished, then OK, finished == true,
        // return OK
        OneRetryTestConfig{grpc::StatusCode::OK, false, grpc::StatusCode::OK,
                           true, grpc::StatusCode::OK},
        // Transient error, then OK, finished == true,
        // return OK
        OneRetryTestConfig{grpc::StatusCode::UNAVAILABLE, false,
                           grpc::StatusCode::OK, true, grpc::StatusCode::OK},
        // Transient error, then still transient from
        // underlying op, finished == true,
        // return UNAVAILABLE
        OneRetryTestConfig{grpc::StatusCode::UNAVAILABLE, false,
                           grpc::StatusCode::UNAVAILABLE, true,
                           grpc::StatusCode::UNAVAILABLE}));

}  // namespace
}  // namespace noex
}  // namespace BIGTABLE_CLIENT_NS
}  // namespace bigtable
}  // namespace cloud
}  // namespace google