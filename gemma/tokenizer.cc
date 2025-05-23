// Copyright 2024 Google LLC
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "gemma/tokenizer.h"

#include <stdio.h>

#include <memory>
#include <string>
#include <vector>

#include "compression/io.h"      // Path
#include "compression/shared.h"  // PromptWrapping
#include "gemma/common.h"        // Wrap
#include "hwy/base.h"              // HWY_ASSERT
#include "hwy/profiler.h"
// copybara:import_next_line:sentencepiece
#include "src/sentencepiece_processor.h"

namespace gcpp {

// Set this to true to debug tokenizer tokens.
constexpr bool kShowTokenization = false;

class GemmaTokenizer::Impl {
 public:
  Impl() = default;
  explicit Impl(const Path& tokenizer_path) {
    PROFILER_ZONE("Startup.tokenizer");
    spp_ = std::make_unique<sentencepiece::SentencePieceProcessor>();
    if (!spp_->Load(tokenizer_path.path).ok()) {
      HWY_ABORT("Failed to load the tokenizer file.");
    }
  }
  // Loads the tokenizer from a serialized proto.
  explicit Impl(const std::string& tokenizer_proto) {
    PROFILER_ZONE("Startup.tokenizer");
    spp_ = std::make_unique<sentencepiece::SentencePieceProcessor>();
    if (!spp_->LoadFromSerializedProto(tokenizer_proto).ok()) {
      fprintf(stderr, "serialized proto size=%zu.\n", tokenizer_proto.size());
      HWY_ABORT("Failed to load the tokenizer from serialized proto.");
    }
  }

  std::string Serialize() const { return spp_->serialized_model_proto(); }

  bool Encode(const std::string& input,
              std::vector<std::string>* pieces) const {
    return spp_ && spp_->Encode(input, pieces).ok();
  }

  bool Encode(const std::string& input, std::vector<int>* ids) const {
    if constexpr (kShowTokenization) {
      bool is_ok = spp_ && spp_->Encode(input, ids).ok();
      for (int i = 0; i < static_cast<int>(ids->size()); i++) {
        fprintf(stderr, "%3d: %d\n", i, (*ids)[i]);
      }
      return is_ok;
    } else {
      return spp_ && spp_->Encode(input, ids).ok();
    }
  }

  // Given a sequence of ids, decodes it into a detokenized output.
  bool Decode(const std::vector<int>& ids, std::string* detokenized) const {
    return spp_ && spp_->Decode(ids, detokenized).ok();
  }

 private:
  std::unique_ptr<sentencepiece::SentencePieceProcessor> spp_;
};

GemmaTokenizer::GemmaTokenizer(const Path& tokenizer_path) {
  impl_ = std::make_unique<Impl>(tokenizer_path);
}

// Default suffices, but they must be defined after GemmaTokenizer::Impl.
GemmaTokenizer::GemmaTokenizer() = default;
GemmaTokenizer::~GemmaTokenizer() = default;
GemmaTokenizer::GemmaTokenizer(GemmaTokenizer&& other) = default;
GemmaTokenizer& GemmaTokenizer::operator=(GemmaTokenizer&& other) = default;

std::string GemmaTokenizer::Serialize() const { return impl_->Serialize(); }

void GemmaTokenizer::Deserialize(const std::string& tokenizer_proto) {
  impl_ = std::make_unique<Impl>(tokenizer_proto);
}

bool GemmaTokenizer::Encode(const std::string& input,
                            std::vector<std::string>* pieces) const {
  return impl_->Encode(input, pieces);
}

bool GemmaTokenizer::Encode(const std::string& input,
                            std::vector<int>* ids) const {
  return impl_->Encode(input, ids);
}

// Given a sequence of ids, decodes it into a detokenized output.
bool GemmaTokenizer::Decode(const std::vector<int>& ids,
                            std::string* detokenized) const {
  return impl_->Decode(ids, detokenized);
}

std::vector<int> WrapAndTokenize(const GemmaTokenizer& tokenizer,
                                 const ModelInfo& info, size_t pos,
                                 std::string& prompt) {
  Wrap(info, pos, prompt);

  std::vector<int> tokens;
  HWY_ASSERT(tokenizer.Encode(prompt, &tokens));
  // Both pre-trained and instruction-tuned require BOS as first token.
  if (pos == 0) {
    tokens.insert(tokens.begin(), BOS_ID);
  }

  // PaliGemma separator. The SEP token "\n" is always tokenized separately.
  if (info.wrapping == PromptWrapping::PALIGEMMA
      // || info.wrapping == PromptWrapping::GEMMA_VLM
  ) {
    std::vector<int> sep_tokens;
    HWY_ASSERT(tokenizer.Encode("\n", &sep_tokens));
    tokens.insert(tokens.end(), sep_tokens.begin(), sep_tokens.end());
  }

  return tokens;
}

std::vector<int> WrapVLM(const GemmaTokenizer& tokenizer, const ModelInfo& info,
                         size_t pos, std::vector<int>& tokens,
                         size_t image_batch_size, size_t max_image_batch_size) {
  HWY_ASSERT(info.wrapping == PromptWrapping::GEMMA_VLM);
  size_t num_images = hwy::DivCeil(image_batch_size, max_image_batch_size);

  std::vector<int> sep_tokens;
  HWY_ASSERT(tokenizer.Encode("\n", &sep_tokens));

  std::string begin_image_prompt = "\n\n<start_of_image>";
  std::vector<int> begin_image_tokens =
      WrapAndTokenize(tokenizer, info, pos, begin_image_prompt);

  std::string end_image_prompt = "<end_of_image>\n\n";
  std::vector<int> end_image_tokens =
      WrapAndTokenize(tokenizer, info, pos, end_image_prompt);

  for (size_t i = 0; i < num_images; ++i) {
    tokens.insert(tokens.begin(), begin_image_tokens.begin(),
                  begin_image_tokens.end());
    tokens.insert(tokens.begin() + begin_image_tokens.size(), image_batch_size,
                  -2);
    tokens.insert(tokens.begin() + begin_image_tokens.size() + image_batch_size,
                  end_image_tokens.begin(), end_image_tokens.end());
  }

  return tokens;
}

}  // namespace gcpp
