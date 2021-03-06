// Copyright 2017 Google LLC
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

#include "riegeli/chunk_encoding/chunk_encoder.h"

#include <utility>

#include "absl/base/optimization.h"
#include "google/protobuf/message_lite.h"
#include "riegeli/base/chain.h"
#include "riegeli/base/status.h"
#include "riegeli/bytes/message_serialize.h"

namespace riegeli {

void ChunkEncoder::Done() {
  num_records_ = 0;
  decoded_data_size_ = 0;
}

bool ChunkEncoder::AddRecord(const google::protobuf::MessageLite& record) {
  if (ABSL_PREDICT_FALSE(!healthy())) return false;
  Chain serialized;
  Status serialize_status = SerializeToChain(record, &serialized);
  if (ABSL_PREDICT_FALSE(!serialize_status.ok())) {
    return Fail(std::move(serialize_status));
  }
  return AddRecord(std::move(serialized));
}

bool ChunkEncoder::AddRecord(Chain&& record) {
  // Not std::move(record): forward to AddRecord(const Chain&).
  return AddRecord(record);
}

}  // namespace riegeli
