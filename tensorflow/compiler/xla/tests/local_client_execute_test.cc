/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <initializer_list>
#include <memory>
#include <vector>

#include "tensorflow/compiler/xla/client/client_library.h"
#include "tensorflow/compiler/xla/client/computation.h"
#include "tensorflow/compiler/xla/client/computation_builder.h"
#include "tensorflow/compiler/xla/client/local_client.h"
#include "tensorflow/compiler/xla/layout_util.h"
#include "tensorflow/compiler/xla/literal_util.h"
#include "tensorflow/compiler/xla/service/device_memory_allocator.h"
#include "tensorflow/compiler/xla/service/local_service.h"
#include "tensorflow/compiler/xla/service/platform_util.h"
#include "tensorflow/compiler/xla/service/shaped_buffer.h"
#include "tensorflow/compiler/xla/service/transfer_manager.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/statusor.h"
#include "tensorflow/compiler/xla/test.h"
#include "tensorflow/compiler/xla/test_helpers.h"
#include "tensorflow/compiler/xla/tests/literal_test_util.h"
#include "tensorflow/compiler/xla/tests/local_client_test_base.h"
#include "tensorflow/compiler/xla/tests/test_macros.h"
#include "tensorflow/compiler/xla/tests/test_utils.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/stream_executor_no_cuda.h"
#include "tensorflow/core/platform/test.h"
#include "tensorflow/core/platform/test_benchmark.h"

namespace se = ::perftools::gputools;

namespace xla {
namespace {

using ::testing::ContainsRegex;

class LocalClientExecuteTest : public LocalClientTestBase {
 protected:
  ErrorSpec error_spec_{0.0001};
};

XLA_TEST_F(LocalClientExecuteTest, Constant) {
  ComputationBuilder builder(local_client_, TestName());
  auto y = builder.ConstantR0<float>(123.0f);

  std::unique_ptr<ScopedShapedBuffer> result =
      ExecuteLocallyOrDie(builder.Build().ValueOrDie(), {});

  LiteralTestUtil::ExpectR0Near<float>(123.f, *ShapedBufferToLiteral(*result),
                                       error_spec_);
}

XLA_TEST_F(LocalClientExecuteTest, AddScalars) {
  ComputationBuilder builder(local_client_, TestName());
  auto x = builder.Parameter(0, ShapeUtil::MakeShape(F32, {}), "x");
  auto y = builder.ConstantR0<float>(123.0f);
  builder.Add(x, y);

  auto x_value = LiteralToScopedShapedBuffer(*Literal::CreateR0<float>(42.0f));
  std::unique_ptr<ScopedShapedBuffer> result =
      ExecuteLocallyOrDie(builder.Build().ValueOrDie(), {x_value.get()});

  LiteralTestUtil::ExpectR0Near<float>(165.f, *ShapedBufferToLiteral(*result),
                                       error_spec_);
}

XLA_TEST_F(LocalClientExecuteTest, AddZeroElementVectors) {
  ComputationBuilder builder(local_client_, TestName());
  auto x = builder.Parameter(0, ShapeUtil::MakeShape(F32, {0}), "x");
  auto y = builder.ConstantR1<float>({});
  builder.Add(x, y);

  auto x_array = LiteralToScopedShapedBuffer(*Literal::CreateR1<float>({}));
  std::unique_ptr<ScopedShapedBuffer> result =
      ExecuteLocallyOrDie(builder.Build().ValueOrDie(), {x_array.get()});

  LiteralTestUtil::ExpectR1Near<float>({}, *ShapedBufferToLiteral(*result),
                                       error_spec_);
}

XLA_TEST_F(LocalClientExecuteTest, AddVectors) {
  ComputationBuilder builder(local_client_, TestName());
  auto x = builder.Parameter(0, ShapeUtil::MakeShape(F32, {3}), "x");
  auto y = builder.ConstantR1<float>({2.0f, 3.0f, 4.0f});
  builder.Add(x, y);

  auto x_array = LiteralToScopedShapedBuffer(
      *Literal::CreateR1<float>({0.0f, 1.0f, 2.0f}));
  std::unique_ptr<ScopedShapedBuffer> result =
      ExecuteLocallyOrDie(builder.Build().ValueOrDie(), {x_array.get()});

  LiteralTestUtil::ExpectR1Near<float>(
      {2.0f, 4.0f, 6.0f}, *ShapedBufferToLiteral(*result), error_spec_);
}

XLA_TEST_F(LocalClientExecuteTest, AddVectorsWithProfile) {
  ComputationBuilder builder(local_client_, TestName());
  auto x = builder.Parameter(0, ShapeUtil::MakeShape(F32, {3}), "x");
  auto y = builder.ConstantR1<float>({2.0f, 3.0f, 4.0f});
  builder.Add(x, y);

  auto x_array = LiteralToScopedShapedBuffer(
      *Literal::CreateR1<float>({0.0f, 1.0f, 2.0f}));
  ExecutionProfile profile;
  std::unique_ptr<ScopedShapedBuffer> result = ExecuteLocallyOrDie(
      builder.Build().ValueOrDie(), {x_array.get()},
      DefaultExecutableBuildOptions(),
      DefaultExecutableRunOptions().set_execution_profile(&profile));

  LiteralTestUtil::ExpectR1Near<float>(
      {2.0f, 4.0f, 6.0f}, *ShapedBufferToLiteral(*result), error_spec_);
  EXPECT_GT(profile.compute_and_transfer_time_ns(), 0);
}

XLA_TEST_F(LocalClientExecuteTest, AddArraysWithDifferentInputLayouts) {
  ComputationBuilder builder(local_client_, TestName());
  auto x = builder.Parameter(0, ShapeUtil::MakeShape(F32, {2, 2}), "x");
  auto y = builder.Parameter(1, ShapeUtil::MakeShape(F32, {2, 2}), "y");
  builder.Add(x, y);
  auto computation = builder.Build().ConsumeValueOrDie();

  // Create x as a col-major array.
  auto x_array = LiteralToScopedShapedBuffer(
      *test_utils::CreateR2LiteralWithLayout({{1.0f, 2.0f}, {3.0f, 4.0f}},
                                             /*minor_to_major=*/{0, 1}));
  EXPECT_TRUE(LayoutUtil::Equal(x_array->shape().layout(),
                                LayoutUtil::MakeLayout({0, 1})));

  // Create y as a row-major array.
  auto y_array = LiteralToScopedShapedBuffer(
      *test_utils::CreateR2LiteralWithLayout({{10.0f, 20.0f}, {30.0f, 40.0f}},
                                             /*minor_to_major=*/{1, 0}));
  EXPECT_TRUE(LayoutUtil::Equal(y_array->shape().layout(),
                                LayoutUtil::MakeLayout({1, 0})));

  std::unique_ptr<ScopedShapedBuffer> result_colmaj =
      ExecuteLocallyOrDie(computation, {x_array.get(), y_array.get()});
  LiteralTestUtil::ExpectR2Near<float>({{11.0f, 22.0f}, {33.0f, 44.0f}},
                                       *ShapedBufferToLiteral(*result_colmaj),
                                       error_spec_);

  // Run with the parameter values in a different order.
  std::unique_ptr<ScopedShapedBuffer> result_param_swap =
      ExecuteLocallyOrDie(computation, {y_array.get(), x_array.get()});
  LiteralTestUtil::ExpectR2Near<float>(
      {{11.0f, 22.0f}, {33.0f, 44.0f}},
      *ShapedBufferToLiteral(*result_param_swap), error_spec_);
}

XLA_TEST_F(LocalClientExecuteTest, AddArraysWithDifferentOutputLayouts) {
  ComputationBuilder builder(local_client_, TestName());
  auto x = builder.Parameter(0, ShapeUtil::MakeShape(F32, {2, 2}), "x");
  auto y = builder.Parameter(1, ShapeUtil::MakeShape(F32, {2, 2}), "y");
  builder.Add(x, y);
  auto computation = builder.Build().ConsumeValueOrDie();

  auto x_array = LiteralToScopedShapedBuffer(
      *Literal::CreateR2<float>({{1.0f, 2.0f}, {3.0f, 4.0f}}));
  auto y_array = LiteralToScopedShapedBuffer(
      *Literal::CreateR2<float>({{10.0f, 20.0f}, {30.0f, 40.0f}}));

  // Run with col-major result layout.
  std::unique_ptr<ScopedShapedBuffer> result_colmaj = ExecuteLocallyOrDie(
      computation, {x_array.get(), y_array.get()},
      DefaultExecutableBuildOptions().set_result_layout(
          ShapeUtil::MakeShapeWithLayout(F32, /*dimensions=*/{2, 2}, {0, 1})),
      DefaultExecutableRunOptions());
  EXPECT_TRUE(LayoutUtil::Equal(result_colmaj->shape().layout(),
                                LayoutUtil::MakeLayout({0, 1})));
  LiteralTestUtil::ExpectR2Near<float>({{11.0f, 22.0f}, {33.0f, 44.0f}},
                                       *ShapedBufferToLiteral(*result_colmaj),
                                       error_spec_);

  // Run with row-major result layout.
  std::unique_ptr<ScopedShapedBuffer> result_rowmaj = ExecuteLocallyOrDie(
      computation, {x_array.get(), y_array.get()},
      DefaultExecutableBuildOptions().set_result_layout(
          ShapeUtil::MakeShapeWithLayout(F32, /*dimensions=*/{2, 2}, {1, 0})),
      DefaultExecutableRunOptions());
  EXPECT_TRUE(LayoutUtil::Equal(result_rowmaj->shape().layout(),
                                LayoutUtil::MakeLayout({1, 0})));
  LiteralTestUtil::ExpectR2Near<float>({{11.0f, 22.0f}, {33.0f, 44.0f}},
                                       *ShapedBufferToLiteral(*result_rowmaj),
                                       error_spec_);
}

XLA_TEST_F(LocalClientExecuteTest, TupleResult) {
  ComputationBuilder builder(local_client_, TestName());
  auto x = builder.Parameter(0, ShapeUtil::MakeShape(F32, {2, 2}), "x");
  auto y = builder.Parameter(1, ShapeUtil::MakeShape(F32, {2, 2}), "y");
  builder.Tuple({x, y, x});
  auto computation = builder.Build().ConsumeValueOrDie();

  auto x_array = LiteralToScopedShapedBuffer(
      *Literal::CreateR2<float>({{1.0f, 2.0f}, {3.0f, 4.0f}}));
  auto y_array = LiteralToScopedShapedBuffer(
      *Literal::CreateR2<float>({{10.0f, 20.0f}, {30.0f, 40.0f}}));

  std::unique_ptr<ScopedShapedBuffer> result =
      ExecuteLocallyOrDie(computation, {x_array.get(), y_array.get()});

  EXPECT_TRUE(ShapeUtil::IsTuple(result->shape()));
  EXPECT_EQ(3, ShapeUtil::TupleElementCount(result->shape()));

  std::unique_ptr<Literal> result_literal = ShapedBufferToLiteral(*result);
  LiteralTestUtil::ExpectR2Equal<float>({{1.0f, 2.0f}, {3.0f, 4.0f}},
                                        result_literal->tuple_literals(0));
  LiteralTestUtil::ExpectR2Equal<float>({{10.0f, 20.0f}, {30.0f, 40.0f}},
                                        result_literal->tuple_literals(1));
  LiteralTestUtil::ExpectR2Equal<float>({{1.0f, 2.0f}, {3.0f, 4.0f}},
                                        result_literal->tuple_literals(2));
}

XLA_TEST_F(LocalClientExecuteTest, NestedTupleResult) {
  ComputationBuilder builder(local_client_, TestName());
  auto x = builder.Parameter(0, ShapeUtil::MakeShape(F32, {2, 2}), "x");
  auto y = builder.Parameter(1, ShapeUtil::MakeShape(F32, {2, 2}), "y");
  auto inner_tuple = builder.Tuple({x, y, x});
  builder.Tuple({inner_tuple, x});
  auto computation = builder.Build().ConsumeValueOrDie();

  auto x_array = LiteralToScopedShapedBuffer(
      *Literal::CreateR2<float>({{1.0f, 2.0f}, {3.0f, 4.0f}}));
  auto y_array = LiteralToScopedShapedBuffer(
      *Literal::CreateR2<float>({{10.0f, 20.0f}, {30.0f, 40.0f}}));

  std::unique_ptr<ScopedShapedBuffer> result =
      ExecuteLocallyOrDie(computation, {x_array.get(), y_array.get()});

  EXPECT_TRUE(ShapeUtil::IsTuple(result->shape()));
  EXPECT_EQ(2, ShapeUtil::TupleElementCount(result->shape()));

  std::unique_ptr<Literal> result_literal = ShapedBufferToLiteral(*result);
  LiteralTestUtil::ExpectR2Equal<float>({{1.0f, 2.0f}, {3.0f, 4.0f}},
                                        result_literal->tuple_literals(1));
  const Literal& inner_tuple_literal = result_literal->tuple_literals(0);
  LiteralTestUtil::ExpectR2Equal<float>({{1.0f, 2.0f}, {3.0f, 4.0f}},
                                        inner_tuple_literal.tuple_literals(0));
  LiteralTestUtil::ExpectR2Equal<float>({{10.0f, 20.0f}, {30.0f, 40.0f}},
                                        inner_tuple_literal.tuple_literals(1));
  LiteralTestUtil::ExpectR2Equal<float>({{1.0f, 2.0f}, {3.0f, 4.0f}},
                                        inner_tuple_literal.tuple_literals(2));
}

XLA_TEST_F(LocalClientExecuteTest, TupleResultWithLayout) {
  // Verify setting the result layout of a computation with a tuple output.
  ComputationBuilder builder(local_client_, TestName());
  auto x = builder.Parameter(0, ShapeUtil::MakeShape(F32, {2, 2}), "x");
  auto y = builder.Parameter(1, ShapeUtil::MakeShape(F32, {2, 2}), "y");
  builder.Tuple({x, y});

  auto array = LiteralToScopedShapedBuffer(
      *Literal::CreateR2<float>({{1.0f, 2.0f}, {3.0f, 4.0f}}));

  ExecutableBuildOptions options = DefaultExecutableBuildOptions();
  Shape shape_with_layout = ShapeUtil::MakeTupleShape(
      {ShapeUtil::MakeShapeWithLayout(F32, /*dimensions=*/{2, 2},
                                      /*minor_to_major=*/{0, 1}),
       ShapeUtil::MakeShapeWithLayout(F32, /*dimensions=*/{2, 2},
                                      /*minor_to_major=*/{1, 0})});
  options.set_result_layout(shape_with_layout);
  std::unique_ptr<ScopedShapedBuffer> result = ExecuteLocallyOrDie(
      builder.Build().ValueOrDie(), {array.get(), array.get()}, options,
      DefaultExecutableRunOptions());

  std::unique_ptr<Literal> result_literal = ShapedBufferToLiteral(*result);
  LiteralTestUtil::ExpectR2Equal<float>({{1.0f, 2.0f}, {3.0f, 4.0f}},
                                        result_literal->tuple_literals(0));
  LiteralTestUtil::ExpectR2Equal<float>({{1.0f, 2.0f}, {3.0f, 4.0f}},
                                        result_literal->tuple_literals(1));
}

XLA_TEST_F(LocalClientExecuteTest, InvalidNumberOfArguments) {
  // Test passing in an invalid number of arguments.
  ComputationBuilder builder(local_client_, TestName());
  auto x = builder.Parameter(0, ShapeUtil::MakeShape(F32, {3}), "x");
  auto y = builder.Parameter(1, ShapeUtil::MakeShape(F32, {3}), "y");
  builder.Add(x, y);

  auto x_array = LiteralToScopedShapedBuffer(
      *Literal::CreateR1<float>({1.0f, 2.0f, 3.0f}));
  auto execute_status =
      ExecuteLocally(builder.Build().ValueOrDie(), {x_array.get()});

  EXPECT_FALSE(execute_status.ok());
  EXPECT_THAT(execute_status.status().error_message(),
              ContainsRegex("invalid number of arguments"));
}

XLA_TEST_F(LocalClientExecuteTest, IncorrectArgumentShape) {
  // Test passing in an argument with the wrong shape.
  ComputationBuilder builder(local_client_, TestName());
  auto x = builder.Parameter(0, ShapeUtil::MakeShape(F32, {3}), "x");
  builder.Neg(x);

  auto x_array = LiteralToScopedShapedBuffer(
      *Literal::CreateR2<float>({{0.0f, 1.0f}, {2.0f, 3.0f}}));
  auto execute_status =
      ExecuteLocally(builder.Build().ValueOrDie(), {x_array.get()});

  EXPECT_FALSE(execute_status.ok());
  EXPECT_THAT(execute_status.status().error_message(),
              ContainsRegex("invalid argument shape"))
      << execute_status.status();
}

XLA_TEST_F(LocalClientExecuteTest, InvalidResultLayout) {
  // Test passing in an invalid result layout parameter.
  ComputationBuilder builder(local_client_, TestName());
  auto x = builder.Parameter(0, ShapeUtil::MakeShape(F32, {2, 2}), "x");
  builder.Neg(x);

  auto x_array = LiteralToScopedShapedBuffer(
      *Literal::CreateR2<float>({{0.0f, 1.0f}, {2.0f, 3.0f}}));
  auto execute_status = ExecuteLocally(
      builder.Build().ValueOrDie(), {x_array.get()},
      DefaultExecutableBuildOptions().set_result_layout(
          ShapeUtil::MakeShapeWithLayout(F32,
                                         /*dimensions=*/{1, 2, 3, 4},
                                         /*minor_to_major=*/{0, 1, 2, 3})),
      DefaultExecutableRunOptions());

  EXPECT_FALSE(execute_status.ok());
  EXPECT_THAT(execute_status.status().error_message(),
              ContainsRegex("not compatible with result shape"))
      << execute_status.status();
}

XLA_TEST_F(LocalClientExecuteTest, RunOnAllDeviceOrdinals) {
  // Try to run a trivial computation on every device on the system. If a
  // specific device is not supported, check that the right error is returned.
  ComputationBuilder builder(local_client_, TestName());
  builder.ConstantR0<float>(42.0f);
  auto computation = builder.Build().ConsumeValueOrDie();
  for (int d = 0; d < local_client_->device_count(); ++d) {
    if (!local_client_->device_ordinal_supported(d)) {
      auto execute_status =
          ExecuteLocally(computation, {},
                         DefaultExecutableBuildOptions().set_device_ordinal(d),
                         DefaultExecutableRunOptions().set_device_ordinal(d));
      EXPECT_FALSE(execute_status.ok());
      EXPECT_THAT(execute_status.status().error_message(),
                  ContainsRegex("device .* not supported"));
    } else {
      auto result = ExecuteLocallyOrDie(
          computation, {},
          DefaultExecutableBuildOptions().set_device_ordinal(d),
          DefaultExecutableRunOptions().set_device_ordinal(d));
      EXPECT_EQ(d, result->device_ordinal());
      LiteralTestUtil::ExpectR0Equal<float>(42.0f,
                                            *ShapedBufferToLiteral(*result));
    }
  }
}

XLA_TEST_F(LocalClientExecuteTest, InvalidDeviceOrdinalValues) {
  // Try running computations on devices with device ordinal values which do not
  // exist.
  ComputationBuilder builder(local_client_, TestName());
  builder.ConstantR0<float>(42.0f);
  auto computation = builder.Build().ConsumeValueOrDie();

  auto execute_status =
      ExecuteLocally(computation, {},
                     DefaultExecutableBuildOptions().set_device_ordinal(
                         local_client_->device_count()),
                     DefaultExecutableRunOptions().set_device_ordinal(
                         local_client_->device_count()));
  EXPECT_FALSE(execute_status.ok());
  EXPECT_THAT(execute_status.status().error_message(),
              ContainsRegex("Invalid device ordinal value"));
}

XLA_TEST_F(LocalClientExecuteTest, RunOnStream) {
  // Run a computation on a specific stream on each device on the system.
  ComputationBuilder builder(local_client_, TestName());
  builder.ConstantR0<float>(42.0f);
  auto computation = builder.Build().ConsumeValueOrDie();

  for (int d = 0; d < local_client_->device_count(); ++d) {
    if (!local_client_->device_ordinal_supported(d)) {
      continue;
    }
    se::StreamExecutor* executor =
        local_client_->platform()->ExecutorForDevice(d).ValueOrDie();
    se::Stream stream(executor);
    stream.Init();

    auto result =
        ExecuteLocallyOrDie(computation, {}, DefaultExecutableBuildOptions(),
                            DefaultExecutableRunOptions().set_stream(&stream));
    // As a check to verify that the computation ran of the device associated
    // with the stream. This is a weak check, but stronger verification is hard.
    EXPECT_EQ(d, result->device_ordinal());
    LiteralTestUtil::ExpectR0Equal<float>(42.0f,
                                          *ShapedBufferToLiteral(*result));
  }
}

// Disable this test on CPU because we're using the CPU as the platform
// which does not match the service platform.
XLA_TEST_F(LocalClientExecuteTest,
           DISABLED_ON_CPU(RunOnStreamForWrongPlatform)) {
  // Try to run a computation on a stream for a platform (CPU) which does not
  // match the platform of the service (!= CPU).
  se::Platform* wrong_platform =
      se::MultiPlatformManager::PlatformWithId(se::host::kHostPlatformId)
          .ValueOrDie();
  se::Stream wrong_stream(wrong_platform->ExecutorForDevice(0).ValueOrDie());
  wrong_stream.Init();

  ComputationBuilder builder(local_client_, TestName());
  builder.ConstantR0<float>(42.0f);
  auto execute_status = ExecuteLocally(
      builder.Build().ValueOrDie(), {}, DefaultExecutableBuildOptions(),
      DefaultExecutableRunOptions().set_stream(&wrong_stream));
  EXPECT_FALSE(execute_status.ok());
  EXPECT_THAT(execute_status.status().error_message(),
              ContainsRegex("stream is for platform .*, but service targets"));
}

XLA_TEST_F(LocalClientExecuteTest,
           DISABLED_ON_CPU(AllocatorDoesNotMatchPlatform)) {
  se::Platform* wrong_platform =
      se::MultiPlatformManager::PlatformWithId(se::host::kHostPlatformId)
          .ValueOrDie();
  TestAllocator allocator(wrong_platform);

  ComputationBuilder builder(local_client_, TestName());
  auto y = builder.ConstantR0<float>(123.0f);

  auto execute_status = ExecuteLocally(
      builder.Build().ValueOrDie(), {}, DefaultExecutableBuildOptions(),
      DefaultExecutableRunOptions().set_allocator(&allocator));
  EXPECT_FALSE(execute_status.ok());
  EXPECT_THAT(execute_status.status().error_message(),
              ContainsRegex("allocator platform .* does not match service"));
}

XLA_TEST_F(LocalClientExecuteTest, RunOnUninitializedStream) {
  // Try to run a computation on a stream that has not been initialized.
  ComputationBuilder builder(local_client_, TestName());
  builder.ConstantR0<float>(42.0f);

  LOG(INFO) << "default device = " << local_client_->default_device_ordinal();
  se::StreamExecutor* executor =
      local_client_->platform()
          ->ExecutorForDevice(local_client_->default_device_ordinal())
          .ValueOrDie();
  se::Stream stream(executor);
  // Don't call stream.Init().

  auto execute_status = ExecuteLocally(
      builder.Build().ValueOrDie(), {}, DefaultExecutableBuildOptions(),
      DefaultExecutableRunOptions().set_stream(&stream));
  EXPECT_FALSE(execute_status.ok());
  EXPECT_THAT(execute_status.status().error_message(),
              ContainsRegex("stream is uninitialized or in an error state"));
}

XLA_TEST_F(LocalClientExecuteTest, SelectBetweenTuples) {
  ComputationBuilder builder(local_client_, TestName());

  std::initializer_list<float> vec1 = {1.f, 2.f, 3.f};
  std::initializer_list<float> vec2 = {2.f, 4.f, 6.f};
  auto tuple12 = builder.Tuple(
      {builder.ConstantR1<float>(vec1), builder.ConstantR1<float>(vec2)});
  auto tuple21 = builder.Tuple(
      {builder.ConstantR1<float>(vec2), builder.ConstantR1<float>(vec1)});
  builder.Select(builder.ConstantR0<bool>(false), tuple12, tuple21);

  std::unique_ptr<ScopedShapedBuffer> result =
      ExecuteLocallyOrDie(builder.Build().ValueOrDie(), {});
  std::unique_ptr<Literal> tuple_literal = ShapedBufferToLiteral(*result);
  LiteralTestUtil::ExpectR1Equal<float>({2.0f, 4.0f, 6.0f},
                                        tuple_literal->tuple_literals(0));
  LiteralTestUtil::ExpectR1Equal<float>({1.0f, 2.0f, 3.0f},
                                        tuple_literal->tuple_literals(1));
}

XLA_TEST_F(LocalClientExecuteTest, CompileExecutable) {
  ComputationBuilder builder(local_client_, TestName());
  auto x = builder.Parameter(0, ShapeUtil::MakeShape(F32, {3}), "x");
  auto y = builder.ConstantR1<float>({2.0f, 3.0f, 4.0f});
  builder.Add(x, y);

  Shape argument_layout =
      ShapeUtil::MakeShapeWithLayout(F32, /*dimensions=*/{3}, {0});
  auto executable_status =
      local_client_->Compile(builder.Build().ValueOrDie(), {&argument_layout},
                             ExecutableBuildOptions());
  ASSERT_IS_OK(executable_status);
  std::unique_ptr<LocalExecutable> executable =
      executable_status.ConsumeValueOrDie();

  auto x_array = LiteralToScopedShapedBuffer(
      *Literal::CreateR1<float>({0.0f, 1.0f, 2.0f}));
  std::unique_ptr<ScopedShapedBuffer> result = ShapedBufferToScopedShapedBuffer(
      executable->Run({x_array.get()}, DefaultExecutableRunOptions())
          .ConsumeValueOrDie(),
      allocator_);

  LiteralTestUtil::ExpectR1Near<float>(
      {2.0f, 4.0f, 6.0f}, *ShapedBufferToLiteral(*result), error_spec_);
}

XLA_TEST_F(LocalClientExecuteTest, ShapeBufferToLiteralConversion) {
  // Test copying Literals to the device as ShapedBuffers, then copying them
  // back again to Literals.
  auto test_to_device_and_back = [this](const Literal& literal) {
    TF_ASSERT_OK_AND_ASSIGN(
        auto shaped_buffer,
        local_client_->LiteralToShapedBuffer(
            literal, allocator_, local_client_->default_device_ordinal()));
    TF_ASSERT_OK_AND_ASSIGN(
        auto transferred_literal,
        local_client_->ShapedBufferToLiteral(*shaped_buffer));
    EXPECT_EQ(literal, *transferred_literal);
  };

  // Array shapes.
  test_to_device_and_back(*Literal::CreateR0<float>(42.0));
  test_to_device_and_back(*Literal::CreateR0<bool>(true));
  test_to_device_and_back(*Literal::CreateR1<float>({1.0, 42.0, 744.4}));
  test_to_device_and_back(
      *Literal::CreateR2<double>({{1.0, 2.0, 3.0}, {44.0, 0.1, -3}}));
  test_to_device_and_back(*Literal::CreateR2<int32>({{2, 1}, {4444, 56}}));

  // Null shape (empty tuple).
  test_to_device_and_back(*Literal::MakeTuple({}));

  // Non-nested tuples.
  test_to_device_and_back(
      *Literal::MakeTuple({Literal::CreateR0<float>(12223.0).get()}));
  test_to_device_and_back(
      *Literal::MakeTuple({Literal::CreateR1<float>({1.0, -42.0}).get(),
                           Literal::CreateR0<float>(123456.0).get()}));

  // Nested tuple.
  test_to_device_and_back(*Literal::MakeTuple(
      {Literal::MakeTuple({Literal::CreateR1<float>({1.0, -42.0}).get(),
                           Literal::CreateR0<float>(123456.0).get()})
           .get(),
       Literal::CreateR0<bool>(false).get()}));
}

// Benchmark that measures the overhead of the LocalClient API when running a
// trivial computation
void BM_LocalClientOverhead(int num_iters) {
  tensorflow::testing::StopTiming();

  se::Platform* platform = PlatformUtil::GetDefaultPlatform().ValueOrDie();
  auto executors = PlatformUtil::GetStreamExecutors(platform).ValueOrDie();
  StreamExecutorMemoryAllocator allocator(platform, executors);
  LocalClient* client =
      ClientLibrary::GetOrCreateLocalClient(platform).ValueOrDie();
  auto* transfer_manager =
      TransferManager::GetForPlatform(platform).ValueOrDie();
  int device_ordinal = client->default_device_ordinal();

  // Use a tiny add operation as the computation.
  ComputationBuilder builder(client, "Add");
  auto shape = ShapeUtil::MakeShape(F32, {2, 3});
  auto x = builder.Parameter(0, shape, "x");
  builder.Add(x, x);
  auto computation = builder.Build().ConsumeValueOrDie();

  auto buffer = ScopedShapedBuffer::MakeScopedShapedBuffer(shape, &allocator, 0)
                    .ConsumeValueOrDie();
  auto literal = Literal::CreateR2<float>({{0, 0, 0}, {0, 0, 0}});
  ASSERT_IS_OK(transfer_manager->TransferLiteralToDevice(
      executors[device_ordinal], *literal, buffer->mutable_buffer({})));

  const int kWarmups = 2;

  auto executable_status = client->Compile(computation, {&buffer->shape()},
                                           ExecutableBuildOptions());
  ASSERT_IS_OK(executable_status);
  std::unique_ptr<LocalExecutable> executable =
      executable_status.ConsumeValueOrDie();

  se::Stream stream(executors[client->default_device_ordinal()]);
  stream.Init();

  ExecutableRunOptions run_options;
  run_options.set_allocator(&allocator).set_stream(&stream);

  for (int i = 0; i < kWarmups; ++i) {
    auto result = executable->Run({buffer.get()}, run_options);
    ASSERT_IS_OK(result);
  }

  tensorflow::testing::StartTiming();
  for (int i = 0; i < num_iters; ++i) {
    auto result = executable->Run({buffer.get()}, run_options);
    ASSERT_IS_OK(result);
  }
}

BENCHMARK(BM_LocalClientOverhead);

}  // namespace
}  // namespace xla