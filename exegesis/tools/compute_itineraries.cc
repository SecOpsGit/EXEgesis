// Copyright 2017 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// A tool to compute itineraries for an instruction set.

#include <functional>
#include <utility>

#include "gflags/gflags.h"

#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_split.h"
#include "exegesis/base/microarchitecture.h"
#include "exegesis/itineraries/compute_itineraries.h"
#include "exegesis/tools/architecture_flags.h"
#include "exegesis/util/proto_util.h"
#include "exegesis/util/system.h"
#include "glog/logging.h"
#include "net/proto2/util/public/repeated_field_util.h"
#include "util/gtl/map_util.h"

DEFINE_string(exegesis_only_llvm_mnemonics, "",
              "If provided, only compute the itineraries for these "
              "instructions (comma-separated list).");
DEFINE_string(exegesis_output_itineraries, "",
              "File where to store the computed itineraries in Proto format.");
DEFINE_int32(exegesis_pin_to_core, 0,
             "Pin the process to the given core. This helps for getting more "
             "reliable results.");

namespace exegesis {

void Main() {
  SetCoreAffinity(FLAGS_exegesis_pin_to_core);

  const auto microarchitecture_data =
      GetMicroArchitectureDataFromCommandLineFlags();

  InstructionSetProto instruction_set =
      microarchitecture_data.instruction_set();
  InstructionSetItinerariesProto itineraries =
      microarchitecture_data.itineraries();

  if (!FLAGS_exegesis_only_llvm_mnemonics.empty()) {
    const absl::flat_hash_set<std::string> mnemonics = absl::StrSplit(
        FLAGS_exegesis_only_llvm_mnemonics, ',', absl::SkipWhitespace());
    RemoveIf(instruction_set.mutable_instructions(),
             [&mnemonics](const InstructionProto* instruction) {
               return !mnemonics.contains(instruction->llvm_mnemonic());
             });
    RemoveIf(itineraries.mutable_itineraries(),
             [&mnemonics](const ItineraryProto* itinerary) {
               return !mnemonics.contains(itinerary->llvm_mnemonic());
             });
  }
  LOG(ERROR) << itineraries::ComputeItineraries(instruction_set, &itineraries);

  WriteTextProtoOrDie(FLAGS_exegesis_output_itineraries, itineraries);
}

}  // namespace exegesis

int main(int argc, char** argv) {
  google::ParseCommandLineFlags(&argc, &argv, true);
  CHECK(!FLAGS_exegesis_output_itineraries.empty())
      << "Please specify the output.";
  exegesis::Main();
  return 0;
}
