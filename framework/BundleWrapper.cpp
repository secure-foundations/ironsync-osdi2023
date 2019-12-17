#include "BundleWrapper.h"
#include "Bundle.cpp"

using namespace MainHandlers_Compile;

std::pair<Constants, Variables> handle_InitState()
{
  auto tup2 = __default::InitState();
  Constants k;
  k.k = shared_ptr<BetreeGraphBlockCache_Compile::Constants>(
      new BetreeGraphBlockCache_Compile::Constants(tup2.t0));
  Variables hs;
  hs.hs = tup2.t1;
  return make_pair(k, hs);
}

DafnyMap<uint64, shared_ptr<vector<uint8>>> handle_InitDiskBytes()
{
  return MkfsImpl_Compile::__default::InitDiskBytes();
}

uint64 handle_PushSync(Constants k, Variables hs, shared_ptr<MainDiskIOHandler_Compile::DiskIOHandler> io)
{
  return __default::handlePushSync(*k.k, hs.hs, io);
}

std::pair<bool, bool> handle_PopSync(Constants k, Variables hs, shared_ptr<MainDiskIOHandler_Compile::DiskIOHandler> io, uint64 id)
{
  auto p = __default::handlePopSync(*k.k, hs.hs, io, id);
  return make_pair(p.t0, p.t1);
}

bool handle_Insert(Constants k, Variables hs, shared_ptr<MainDiskIOHandler_Compile::DiskIOHandler> io, DafnySequence<uint8> key, DafnySequence<uint8> value)
{
  return __default::handleInsert(*k.k, hs.hs, io, key, value);
}

std::pair<bool, DafnySequence<uint8>> handle_Query(Constants k, Variables hs, shared_ptr<MainDiskIOHandler_Compile::DiskIOHandler> io, DafnySequence<uint8> key)
{
  auto p = __default::handleQuery(*k.k, hs.hs, io, key);
  return make_pair(p.is_Some(), p.v_Some.value);
}

void handle_ReadResponse(Constants k, Variables hs, shared_ptr<MainDiskIOHandler_Compile::DiskIOHandler> io)
{
  __default::handleReadResponse(*k.k, hs.hs, io);
}

void handle_WriteResponse(Constants k, Variables hs, shared_ptr<MainDiskIOHandler_Compile::DiskIOHandler> io)
{
  __default::handleWriteResponse(*k.k, hs.hs, io);
}

uint64 MaxKeyLen()
{
  return KeyType_Compile::__default::MaxLen();
}

uint64 MaxValueLen()
{
  return ValueWithDefault_Compile::__default::MaxLen();
}