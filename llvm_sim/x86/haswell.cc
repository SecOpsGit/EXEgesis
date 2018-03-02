// Copyright 2018 Google Inc.
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

#include "llvm_sim/x86/haswell.h"

#include "llvm_sim/components/buffer.h"
#include "llvm_sim/components/decoder.h"
#include "llvm_sim/components/dispatch_port.h"
#include "llvm_sim/components/execution_unit.h"
#include "llvm_sim/components/fetcher.h"
#include "llvm_sim/components/parser.h"
#include "llvm_sim/components/port.h"
#include "llvm_sim/components/register_renamer.h"
#include "llvm_sim/components/reorder_buffer.h"
#include "llvm_sim/components/retirer.h"
#include "llvm_sim/components/simplified_execution_units.h"

namespace exegesis {
namespace simulator {

namespace {

constexpr const size_t kInfiniteCapacity = std::numeric_limits<size_t>::max();

}  // namespace

std::unique_ptr<Simulator> CreateHaswellSimulator(
    const GlobalContext& Context) {
  // Create Buffers ------------------------------------------------------------
  // "Instruction Queue", a.k.a. "Pre-Decode Buffer".
  auto InstructionQueue = llvm::make_unique<FifoBuffer<InstructionIndex>>(20);
  // "Instruction Decode Queue", a.k.a. "IDQ", "uOp Queue".
  auto InstructionDecodeQueue = llvm::make_unique<FifoBuffer<UopId>>(64);
  // Ports.
  std::vector<std::unique_ptr<LinkBuffer<ROBUopId>>> Ports;
  std::vector<Sink<ROBUopId>*> PortSinks;
  std::vector<std::string> PortNames;
  for (int ProcResIdx = 1;
       ProcResIdx < Context.SchedModel->getNumProcResourceKinds();
       ++ProcResIdx) {
    const llvm::MCProcResourceDesc* const ProcResDesc =
        Context.SchedModel->getProcResource(ProcResIdx);
    if (ProcResDesc->SubUnitsIdxBegin == nullptr) {
      // NumUnits is the number of units of a ProcResource. For example,
      // SandyBridge has:
      //   def SBPort23 : ProcResource<2>;
      // i.e. it models the two ports as a single resource with two units.
      // As for as the simulator is concerned, this is similar to having two
      // ports with one unit, but the reorder buffer dispatches by resource id.
      Ports.push_back(
          llvm::make_unique<DispatchPort<ROBUopId>>(ProcResDesc->NumUnits));
      PortSinks.push_back(Ports.back().get());
#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
      PortNames.push_back(ProcResDesc->Name);
#else
      // TODO(courbet): Better port naming in release mode.
      PortNames.push_back(
          llvm::Twine("ProcResIdx ").concat(llvm::Twine(ProcResIdx)).str());
#endif
    }
  }
  // Links.
  // Fetched instructions buffer.
  auto FetchedInstructionsLink =
      llvm::make_unique<LinkBuffer<InstructionIndex>>(kInfiniteCapacity);
  auto RenamerToROBLink =
      llvm::make_unique<LinkBuffer<RenamedUopId>>(kInfiniteCapacity);
  // ROB->Retirer and Retirer->ROB writeback links.
  auto UopsToRetireLink = llvm::make_unique<LinkBuffer<ROBUopId>>(3);
  auto RetiredUopsLink =
      llvm::make_unique<LinkBuffer<ROBUopId>>(kInfiniteCapacity);
  auto ExecDepsTracker = llvm::make_unique<ExecDepsBuffer<ROBUopId>>();
  // Executed uops writeback link.
  auto ExecutedWritebackLink =
      llvm::make_unique<LinkBuffer<ROBUopId>>(kInfiniteCapacity);

  // Create and add components -------------------------------------------------
  auto Simulator = llvm::make_unique<class Simulator>();

  // Instruction Fetcher.
  Simulator->AddComponent(llvm::make_unique<Fetcher>(
      &Context, Fetcher::Config{16}, FetchedInstructionsLink.get()));
  // Instruction Parser.
  Simulator->AddComponent(llvm::make_unique<InstructionParser>(
      &Context, InstructionParser::Config{4}, FetchedInstructionsLink.get(),
      InstructionQueue.get()));
  // Instruction Decoder.
  Simulator->AddComponent(llvm::make_unique<InstructionDecoder>(
      &Context, InstructionDecoder::Config{5}, InstructionQueue.get(),
      InstructionDecodeQueue.get()));
  // Register Renamer.
  Simulator->AddComponent(llvm::make_unique<RegisterRenamer>(
      &Context, RegisterRenamer::Config{3, 1000000},
      InstructionDecodeQueue.get(), RenamerToROBLink.get()));
  // Reorder Buffer.
  Simulator->AddComponent(llvm::make_unique<ReorderBuffer>(
      &Context, ReorderBuffer::Config{/*NumROBEntries=*/192},
      RenamerToROBLink.get(), ExecDepsTracker.get(),
      ExecutedWritebackLink.get(), RetiredUopsLink.get(), ExecDepsTracker.get(),
      PortSinks, UopsToRetireLink.get(), IssuePolicy::LeastLoaded()));
  // Execution units.
  for (int Port = 0; Port < Ports.size(); ++Port) {
    Simulator->AddComponent(
        llvm::make_unique<SimplifiedExecutionUnits<ROBUopId>>(
            &Context, SimplifiedExecutionUnits<ROBUopId>::Config{},
            Ports[Port].get(), ExecutedWritebackLink.get()));
  }
  // Retirement Station.
  Simulator->AddComponent(llvm::make_unique<Retirer<ROBUopId>>(
      &Context, Retirer<ROBUopId>::Config{}, UopsToRetireLink.get(),
      RetiredUopsLink.get(), Simulator->GetInstructionSink()));

  // Add Buffers ---------------------------------------------------------------
  Simulator->AddBuffer(std::move(FetchedInstructionsLink), {"FetchBuffer"});
  Simulator->AddBuffer(std::move(InstructionQueue), {"Pre-Decode Buffer"});
  Simulator->AddBuffer(std::move(InstructionDecodeQueue),
                       {"Instruction Decode Queue"});
  for (int I = 0; I < Ports.size(); ++I) {
    Simulator->AddBuffer(std::move(Ports[I]), {PortNames[I]});
  }
  Simulator->AddBuffer(std::move(RenamerToROBLink), {"Renamed Uops"});
  Simulator->AddBuffer(std::move(UopsToRetireLink), {"Ready to Retire Uops"});
  Simulator->AddBuffer(std::move(ExecutedWritebackLink), {"ROB Writeback"});
  Simulator->AddBuffer(std::move(ExecDepsTracker), {"Outputs Available"});
  Simulator->AddBuffer(std::move(RetiredUopsLink), {"Retired Uops"});

  return Simulator;
}

}  // namespace simulator
}  // namespace exegesis
