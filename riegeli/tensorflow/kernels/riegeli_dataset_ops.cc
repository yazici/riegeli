// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stddef.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"
#include "absl/types/optional.h"
#include "riegeli/base/base.h"
#include "riegeli/base/status.h"
#include "riegeli/records/record_position.h"
#include "riegeli/records/record_reader.h"
#include "riegeli/tensorflow/io/file_reader.h"
#include "tensorflow/core/framework/allocator.h"
#include "tensorflow/core/framework/dataset.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/framework/types.h"
#include "tensorflow/core/framework/types.pb.h"
#include "tensorflow/core/graph/graph.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/platform/macros.h"
#include "tensorflow/core/platform/types.h"

namespace riegeli {
namespace tensorflow {
namespace {

class RiegeliDatasetOp : public ::tensorflow::DatasetOpKernel {
 public:
  using DatasetOpKernel::DatasetOpKernel;

  void MakeDataset(::tensorflow::OpKernelContext* ctx,
                   ::tensorflow::DatasetBase** output) override {
    const ::tensorflow::Tensor* filenames_tensor;
    OP_REQUIRES_OK(ctx, ctx->input("filenames", &filenames_tensor));
    OP_REQUIRES(ctx, filenames_tensor->dims() <= 1,
                ::tensorflow::errors::InvalidArgument(
                    "`filenames` must be a scalar or a vector."));

    std::vector<std::string> filenames;
    filenames.reserve(IntCast<size_t>(filenames_tensor->NumElements()));
    for (int i = 0; i < filenames_tensor->NumElements(); ++i) {
      filenames.push_back(filenames_tensor->flat<std::string>()(i));
    }

    *output = new Dataset(ctx, std::move(filenames));
  }

 private:
  class Dataset : public ::tensorflow::DatasetBase {
   public:
    explicit Dataset(::tensorflow::OpKernelContext* ctx,
                     std::vector<std::string> filenames)
        : DatasetBase(::tensorflow::DatasetContext(ctx)),
          filenames_(std::move(filenames)) {}

    std::unique_ptr<::tensorflow::IteratorBase> MakeIteratorInternal(
        const std::string& prefix) const override {
      return std::unique_ptr<::tensorflow::IteratorBase>(
          new Iterator({this, absl::StrCat(prefix, "::Riegeli")}));
    }

    const ::tensorflow::DataTypeVector& output_dtypes() const override {
      static const ::tensorflow::DataTypeVector* const dtypes =
          new ::tensorflow::DataTypeVector({::tensorflow::DT_STRING});
      return *dtypes;
    }

    const std::vector<::tensorflow::PartialTensorShape>& output_shapes()
        const override {
      static const std::vector<::tensorflow::PartialTensorShape>* const shapes =
          new std::vector<::tensorflow::PartialTensorShape>({{}});
      return *shapes;
    }

    std::string DebugString() const override {
      return "RiegeliDatasetOp::Dataset";
    }

   protected:
    ::tensorflow::Status AsGraphDefInternal(
        ::tensorflow::SerializationContext* ctx, DatasetGraphDefBuilder* b,
        ::tensorflow::Node** output) const override {
      ::tensorflow::Node* filenames = nullptr;
      TF_RETURN_IF_ERROR(b->AddVector(filenames_, &filenames));
      TF_RETURN_IF_ERROR(b->AddDataset(this, {filenames}, output));
      return ::tensorflow::Status::OK();
    }

   private:
    class Iterator : public ::tensorflow::DatasetIterator<Dataset> {
     public:
      explicit Iterator(const Params& params)
          : DatasetIterator<Dataset>(params) {}

      ::tensorflow::Status GetNextInternal(
          ::tensorflow::IteratorContext* ctx,
          std::vector<::tensorflow::Tensor>* out_tensors,
          bool* end_of_sequence) override LOCKS_EXCLUDED(mu_) {
        absl::MutexLock l(&mu_);
        for (;;) {
          if (reader_.has_value()) {
            // We are currently processing a file, so try to read the next
            // record.
            ::tensorflow::Tensor result_tensor(::tensorflow::cpu_allocator(),
                                               ::tensorflow::DT_STRING, {});
            std::string* const value = &result_tensor.scalar<std::string>()();
            if (TF_PREDICT_TRUE(reader_->ReadRecord(value))) {
              out_tensors->push_back(std::move(result_tensor));
              *end_of_sequence = false;
              return ::tensorflow::Status::OK();
            }
            SkippedRegion skipped_region;
            if (reader_->Recover(&skipped_region)) {
              // File has invalid contents: return an error. Further iteration
              // will resume reading the file after the invalid region has been
              // skipped.
              *end_of_sequence = false;
              return ::tensorflow::errors::DataLoss(
                  "Skipping invalid region of a Riegeli/records file: ",
                  skipped_region.ToString());
            }
            if (TF_PREDICT_FALSE(!reader_->Close())) {
              // Failed to read the file: return an error.
              const Status status = reader_->status();
              // Further iteration will move on to the next file, if any.
              reader_.reset();
              ++current_file_index_;
              *end_of_sequence =
                  current_file_index_ == dataset()->filenames_.size();
              return ::tensorflow::Status(
                  static_cast<::tensorflow::error::Code>(status.code()),
                  status.message());
            }
            // We have reached the end of the current file, so move on to the
            // next file, if any.
            reader_.reset();
            ++current_file_index_;
          }

          // Iteration ends when there are no more files to process.
          if (current_file_index_ == dataset()->filenames_.size()) {
            *end_of_sequence = true;
            return ::tensorflow::Status::OK();
          }

          // Actually move on to next file.
          OpenFile(ctx);
        }
      }

     protected:
      ::tensorflow::Status SaveInternal(
          ::tensorflow::IteratorStateWriter* writer) override
          LOCKS_EXCLUDED(mu_) {
        absl::MutexLock l(&mu_);
        TF_RETURN_IF_ERROR(writer->WriteScalar(
            full_name("current_file_index"),
            IntCast<::tensorflow::int64>(current_file_index_)));
        if (reader_.has_value()) {
          TF_RETURN_IF_ERROR(writer->WriteScalar(full_name("current_pos"),
                                                 reader_->pos().ToBytes()));
        }
        return ::tensorflow::Status::OK();
      }

      ::tensorflow::Status RestoreInternal(
          ::tensorflow::IteratorContext* ctx,
          ::tensorflow::IteratorStateReader* reader) override
          LOCKS_EXCLUDED(mu_) {
        absl::MutexLock l(&mu_);
        current_file_index_ = 0;
        reader_.reset();

        ::tensorflow::int64 current_file_index;
        TF_RETURN_IF_ERROR(reader->ReadScalar(full_name("current_file_index"),
                                              &current_file_index));
        if (TF_PREDICT_FALSE(current_file_index < 0 ||
                             IntCast<::tensorflow::uint64>(current_file_index) >
                                 dataset()->filenames_.size())) {
          return ::tensorflow::errors::Internal(
              "current_file_index out of range");
        }
        current_file_index_ = IntCast<size_t>(current_file_index);

        if (reader->Contains(full_name("current_pos"))) {
          if (TF_PREDICT_FALSE(current_file_index_ ==
                               dataset()->filenames_.size())) {
            return ::tensorflow::errors::Internal(
                "current_file_index out of range");
          }
          std::string current_pos;
          TF_RETURN_IF_ERROR(
              reader->ReadScalar(full_name("current_pos"), &current_pos));
          RecordPosition pos;
          if (TF_PREDICT_FALSE(!pos.FromBytes(current_pos))) {
            return ::tensorflow::errors::Internal(
                "current_pos is not a valid RecordPosition");
          }
          OpenFile(ctx);
          reader_->Seek(pos);
          // Any errors from seeking will be reported during reading.
        }
        return ::tensorflow::Status::OK();
      }

     private:
      void OpenFile(::tensorflow::IteratorContext* ctx)
          EXCLUSIVE_LOCKS_REQUIRED(mu_) {
        reader_.emplace(tensorflow::FileReader<>(
            dataset()->filenames_[current_file_index_],
            tensorflow::FileReaderBase::Options().set_env(ctx->env())));
      }

      // Invariants:
      //   current_file_index_ <= dataset()->filenames_.size()
      //   if current_file_index_ == dataset()->filenames_.size() then
      //       !reader_.has_value()

      absl::Mutex mu_;
      size_t current_file_index_ GUARDED_BY(mu_) = 0;
      // nullopt means not open yet.
      absl::optional<RecordReader<tensorflow::FileReader<>>> reader_
          GUARDED_BY(mu_);
    };

    const std::vector<std::string> filenames_;
  };
};

REGISTER_KERNEL_BUILDER(Name("RiegeliDataset").Device(::tensorflow::DEVICE_CPU),
                        RiegeliDatasetOp);

}  // namespace
}  // namespace tensorflow
}  // namespace riegeli
