include "JournalistModel.i.dfy"
include "../lib/Base/NativeArrays.s.dfy"
include "JournalistMarshallingImpl.i.dfy"

module JournalistImpl {
  import opened DiskLayout
  import opened NativeTypes
  import opened Options
  import opened Sequences
  import NativeArrays

  import opened JournalRanges`Internal
  import opened JournalBytes
  import opened Journal
  import JournalistMarshallingModel
  import opened JournalistMarshallingImpl

  import JournalistModel

  class Journalist {
    var journalEntries: array<JournalEntry>;
    var start: uint64;
    var len1: uint64;
    var len2: uint64;

    var replayJournal: seq<JournalEntry>;
    var replayIdx: uint64;

    var journalFront: Option<seq<byte>>;
    var journalBack: Option<seq<byte>>;
    
    var writtenJournalBlocks: uint64;
    var frozenJournalBlocks: uint64;
    var inMemoryWeight: uint64;

    ghost var Repr: set<object>;

    protected predicate ReprInv()
    reads this
    ensures ReprInv() ==> this in Repr
    {
      Repr == {this, this.journalEntries}
    }

    protected function I() : JournalistModel.JournalistModel
    reads this, this.Repr
    requires ReprInv()
    {
      JournalistModel.JournalistModel(
        this.journalEntries[..],
        this.start,
        this.len1,
        this.len2,
        this.replayJournal,
        this.replayIdx,
        this.journalFront,
        this.journalBack,
        this.writtenJournalBlocks,
        this.frozenJournalBlocks,
        this.inMemoryWeight)
    }

    predicate WF()
    reads this, this.Repr
    {
      && ReprInv()
      && JournalistModel.WF(I())
    }

    protected predicate Inv()
    reads this, this.Repr
    ensures Inv() ==> ReprInv()
    {
      && ReprInv()
      && JournalistModel.Inv(I())
    }

    constructor()
    ensures Inv()
    ensures fresh(Repr)
    ensures I() == JournalistModel.JournalistConstructor()
    {
      new;
      this.journalEntries := NativeArrays.newArrayFill(
          JournalistModel.Len(),
          JournalInsert([], []));
      this.start := 0;
      this.len1 := 0;
      this.len2 := 0;
      this.replayJournal := [];
      this.replayIdx := 0;
      this.journalFront := None;
      this.journalBack := None;
      this.writtenJournalBlocks := 0;
      this.frozenJournalBlocks := 0;
      this.inMemoryWeight := 0;

      Repr := {this, this.journalEntries};
      JournalistModel.reveal_JournalistConstructor();
      assert I() == JournalistModel.JournalistConstructor();
    }

    method hasFrozenJournal() returns (b: bool)
    requires Inv()
    ensures b == JournalistModel.hasFrozenJournal(I())
    {
      JournalistModel.reveal_hasFrozenJournal();
      return this.len1 != 0;
    }

    method hasInMemoryJournal() returns (b: bool)
    requires Inv()
    ensures b == JournalistModel.hasInMemoryJournal(I())
    {
      JournalistModel.reveal_hasInMemoryJournal();
      return this.len2 != 0;
    }

    method packageFrozenJournal() returns (s: seq<byte>)
    requires Inv()
    requires JournalistModel.packageFrozenJournal.requires(I())
    modifies this.Repr
    ensures Repr == old(Repr)
    ensures Inv()
    ensures (I(), s) == JournalistModel.packageFrozenJournal(old(I()))
    {
      JournalistModel.reveal_packageFrozenJournal();
      reveal_WeightJournalEntries();
      JournalistModel.reveal_I(I());

      s := MarshallJournalEntries(
        this.journalEntries,
        this.start,
        this.len1,
        this.frozenJournalBlocks);

      this.start := JournalistModel.basic_mod(this.start + this.len1);
      this.len1 := 0;
      this.writtenJournalBlocks :=
          this.writtenJournalBlocks + this.frozenJournalBlocks;
      this.frozenJournalBlocks := 0;
    }

    method packageInMemoryJournal() returns (s: seq<byte>)
    requires Inv()
    requires JournalistModel.packageInMemoryJournal.requires(I())
    modifies this.Repr
    ensures Repr == old(Repr)
    ensures Inv()
    ensures (I(), s) == JournalistModel.packageInMemoryJournal(old(I()))
    {
      JournalistModel.reveal_packageInMemoryJournal();
      reveal_WeightJournalEntries();
      JournalistModel.reveal_I(I());

      var numBlocks := (this.inMemoryWeight + 4064 - 1) / 4064;
      s := MarshallJournalEntries(
        this.journalEntries,
        this.start,
        this.len2,
        numBlocks);

      this.start := 0;
      this.len2 := 0;
      this.inMemoryWeight := 0;
      this.writtenJournalBlocks := this.writtenJournalBlocks + numBlocks;
    }

    method getWrittenJournalLen()
    returns (len : uint64)
    requires Inv()
    ensures len == JournalistModel.getWrittenJournalLen(I())
    {
      return this.writtenJournalBlocks;
    }

    method setWrittenJournalLen(len: uint64)
    requires Inv()
    requires JournalistModel.setWrittenJournalLen.requires(I(), len)
    modifies Repr
    ensures Repr == old(Repr)
    ensures Inv()
    ensures I() == JournalistModel.setWrittenJournalLen(old(I()), len)
    {
      this.writtenJournalBlocks := len;
      this.frozenJournalBlocks := 0;
      assert I() == JournalistModel.setWrittenJournalLen(old(I()), len);
    }

    method updateWrittenJournalLen(len: uint64)
    requires Inv()
    requires JournalistModel.updateWrittenJournalLen.requires(I(), len)
    modifies Repr
    ensures Repr == old(Repr)
    ensures Inv()
    ensures I() == JournalistModel.updateWrittenJournalLen(old(I()), len)
    {
      this.writtenJournalBlocks := len;
      assert I() ==
        JournalistModel.updateWrittenJournalLen(old(I()), len);
    }

    method freeze()
    requires Inv()
    modifies Repr
    ensures Repr == old(Repr)
    ensures Inv()
    ensures I() == JournalistModel.freeze(old(I()))
    {
      JournalistModel.reveal_freeze();

      this.len1 := this.len1 + this.len2;
      this.len2 := 0;
      this.frozenJournalBlocks := this.frozenJournalBlocks
          + (this.inMemoryWeight + 4064 - 1) / 4064;
      this.inMemoryWeight := 0;

      assert I() == JournalistModel.freeze(old(I()));
    }

    method canAppend(je: JournalEntry)
    returns (b: bool)
    requires Inv()
    ensures b == JournalistModel.canAppend(I(), je)
    {
      JournalistModel.reveal_canAppend();

      b := 4064 * (writtenJournalBlocks + frozenJournalBlocks)
          + inMemoryWeight
          + WeightJournalEntryUint64(je)
          + (if len2 == 0 then 8 else 0)
        <= 4064 * NumJournalBlocks();
    }

    method append(je: JournalEntry)
    requires Inv()
    requires JournalistModel.canAppend(I(), je)
    modifies Repr
    ensures Repr == old(Repr)
    ensures Inv()
    ensures I() == JournalistModel.append(old(I()), je)
    {
      JournalistModel.reveal_append();

      var idx := JournalistModel.basic_mod(start + len1 + len2);
      this.journalEntries[idx] := je;
      this.inMemoryWeight := this.inMemoryWeight
          + WeightJournalEntryUint64(je)
          + (if this.len2 == 0 then 8 else 0);
      this.len2 := this.len2 + 1;

      assert I() == JournalistModel.append(old(I()), je);
    }

    method isReplayEmpty()
    returns (b: bool)
    requires Inv()
    ensures b == JournalistModel.isReplayEmpty(I())
    {
      JournalistModel.reveal_isReplayEmpty();
      b := (replayIdx == |replayJournal| as uint64);
    }
  }
}