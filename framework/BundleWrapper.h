#pragma once
#include <memory>
#include "Framework.h"

namespace BetreeGraphBlockCache_Compile {
  struct Constants;
}
namespace MainHandlers_Compile {
  class HeapState;
}

struct Constants {
  std::shared_ptr<BetreeGraphBlockCache_Compile::Constants> k;
};

struct Variables {
  std::shared_ptr<MainHandlers_Compile::HeapState> hs;
};

std::pair<Constants, Variables> handle_InitState();
DafnyMap<uint64, shared_ptr<vector<uint8>>> handle_InitDiskBytes();
uint64 handle_PushSync(Constants, Variables, shared_ptr<MainDiskIOHandler_Compile::DiskIOHandler>);
std::pair<bool, bool> handle_PopSync(Constants, Variables, shared_ptr<MainDiskIOHandler_Compile::DiskIOHandler>, uint64);
bool handle_Insert(Constants, Variables, shared_ptr<MainDiskIOHandler_Compile::DiskIOHandler>, DafnySequence<uint8>, DafnySequence<uint8>);
std::pair<bool, DafnySequence<uint8>> handle_Query(Constants, Variables, shared_ptr<MainDiskIOHandler_Compile::DiskIOHandler>, DafnySequence<uint8>);
void handle_ReadResponse(Constants, Variables, shared_ptr<MainDiskIOHandler_Compile::DiskIOHandler>);
void handle_WriteResponse(Constants, Variables, shared_ptr<MainDiskIOHandler_Compile::DiskIOHandler>);

uint64 MaxKeyLen();
uint64 MaxValueLen();