// RUN: %dafny /compile:0 /verifySnapshots:2 /traceCaching:1 Inputs/Snapshots7.dfy > "%t"
// RUN: %diff "%s.expect" "%t"
// XFAIL: *
// FIXME - need to regenerate the snapshots
