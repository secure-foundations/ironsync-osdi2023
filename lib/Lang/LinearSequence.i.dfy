include "LinearMaybe.s.dfy"
include "LinearSequence.s.dfy"

module LinearSequence_i {
  import opened NativeTypes
  import opened LinearMaybe
  import opened LinearSequence_s
  export
    provides LinearSequence_s
    provides NativeTypes
    provides seq_alloc_init, lseqs, imagine_lseq, lseq_has, lseq_length, lseq_peek
    provides lseq_alloc, lseq_free, lseq_swap, lseq_take, lseq_give
    provides AllocAndCopy, AllocAndMoveLseq, ImagineInverse
    reveals lseq_full, linLast, lseq_has_all
    reveals operator'cardinality?lseq, operator'in?lseq, operator'subscript?lseq

  // method seq_alloc_init<A>(length:nat, a:A) returns(linear s:seq<A>)
  //     ensures |s| == length
  //     ensures forall i:nat | i < |s| :: s[i] == a
  // {
  //     s := seq_alloc(length);
  //     var n := 0;
  //     while (n < length)
  //         invariant |s| == length;
  //         invariant n <= length;
  //         invariant forall i:nat | i < n :: s[i] == a
  //     {
  //         s := seq_set(s, n, a);
  //         n := n + 1;
  //     }
  // }

  function method seq_alloc_init_iterate<A>(length:uint64, a:A, i:uint64, linear sofar:seq<A>) : (linear s:seq<A>)
    requires i<=length;
    requires |sofar| == length as nat;
    requires forall j:nat | j < i as nat :: sofar[j] == a
    ensures |s| == length as nat;
    ensures forall j:nat | j < length as nat :: s[j] == a
    decreases length - i;
  {
    if i == length then
      sofar
    else
      seq_alloc_init_iterate(length, a, i + 1, seq_set(sofar, i, a))
  }

  function method seq_alloc_init<A>(length:uint64, a:A) : (linear s:seq<A>)
      ensures |s| == length as int
      ensures forall i:nat | i < |s| :: s[i] == a
  {
    seq_alloc_init_iterate(length, a, 0, seq_alloc(length))
  }

  function lseqs<A>(l:lseq<A>):(s:seq<A>)
    ensures rank_is_less_than(s, l)
  {
    var s := seq(lseq_length_raw(l), i requires 0<=i<lseq_length_raw(l) => read(lseqs_raw(l)[i]));
    axiom_lseqs_rank(l, s);
    s
  }

  function imagine_lseq<A>(s:seq<A>):(l:lseq<A>)
    ensures lseqs(l) == s
  {
    imagine_lseq_raw(s)
  }

  lemma ImagineInverse<A>(l:lseq<A>)
    ensures imagine_lseq(lseqs(l)) == l
  {
    // TODO(jonh) uh, -- TODO(chris)?
  }

  function linLast<A>(l:lseq<A>) : A
    requires 0<|l|
  {
    lseqs(l)[|l| - 1]
  }

  function lseq_has<A>(l:lseq<A>):(s:seq<bool>)
      ensures |s| == |lseqs(l)|
  {
      seq(lseq_length_raw(l), i requires 0<=i<lseq_length_raw(l) => has(lseqs_raw(l)[i]))
  }

  predicate lseq_has_all<A>(l:lseq<A>)
  {
    forall i :: 0<=i<|l| ==> lseq_has(l)[i]
  }

  function method lseq_length<A>(shared s:lseq<A>):(n:nat)
      ensures n == |lseqs(s)|
  {
      lseq_length_raw(s)
  }

  function method{:inline true} operator(| |)<A>(shared s:seq<A>):nat
  {
      seq_length(s)
  }

  function method{:inline true} operator(| |)<A>(shared s:lseq<A>):nat
  {
      lseq_length(s)
  }

  function{:inline true} operator([])<A>(s:lseq<A>, i:nat):A
      requires i < |s|
  {
      lseqs(s)[i]
  }

  function{:inline true} operator(in)<A>(s:lseq<A>, i:nat):bool
      requires i < |s|
  {
      lseq_has(s)[i]
  }

  function method lseq_peek<A>(shared s:lseq<A>, i:uint64):(shared a:A)
      requires i as nat < |s| && i as nat in s
      ensures a == s[i as nat]
  {
      peek(lseq_share_raw(s, i))
  }

  method lseq_alloc<A>(length:uint64) returns(linear s:lseq<A>)
      ensures |s| == length as nat
      ensures forall i:nat | i < length as nat :: i !in s
  {
      s := lseq_alloc_raw(length);
  }

  method lseq_free<A>(linear s:lseq<A>)
      requires forall i:nat | i < |s| :: i !in s
  {
      assert forall i:nat {:trigger lseqs_raw(s)[i]} | i < |lseqs_raw(s)| :: i !in s;
      var _ := lseq_free_raw(s);
  }

  // can be implemented as in-place swap
  method lseq_swap<A>(linear s1:lseq<A>, i:uint64, linear a1:A) returns(linear s2:lseq<A>, linear a2:A)
      requires i as nat < |s1| && i as nat in s1
      ensures a2 == s1[i as nat]
      ensures lseq_has(s2) == lseq_has(s1)
      ensures lseqs(s2) == lseqs(s1)[i as nat := a1]
  {
      linear var x1:maybe<A> := give(a1);
      linear var x2:maybe<A>;
      s2, x2 := lseq_swap_raw(s1, i, x1);
      a2 := unwrap(x2);
  }

  method lseq_take<A>(linear s1:lseq<A>, i:uint64) returns(linear s2:lseq<A>, linear a:A)
      requires i as nat < |s1| && i as nat in s1
      ensures a == s1[i as nat]
      ensures lseq_has(s2) == lseq_has(s1)[i as nat := false]
      ensures forall j:nat | j < |s1| && j != i as nat :: lseqs(s2)[j] == lseqs(s1)[j]
  {
      linear var x1:maybe<A> := empty();
      linear var x2:maybe<A>;
      s2, x2 := lseq_swap_raw(s1, i, x1);
      a := unwrap(x2);
  }

  method lseq_give<A>(linear s1:lseq<A>, i:uint64, linear a:A) returns(linear s2:lseq<A>)
      requires i as nat < |s1|
      requires i as nat !in s1
      ensures lseq_has(s2) == lseq_has(s1)[i as nat := true]
      ensures lseqs(s2) == lseqs(s1)[i as nat := a]
  {
      linear var x1:maybe<A> := give(a);
      linear var x2:maybe<A>;
      s2, x2 := lseq_swap_raw(s1, i, x1);
      var _ := discard(x2);
  }

  predicate lseq_full<A>(s: lseq<A>)
  {
      && (forall i | 0 <= i < |s| :: i in s)
  }

  // TODO(robj): "probably not as fast as a memcpy"
  method AllocAndCopy<A>(shared source: seq<A>, from: uint64, to: uint64)
    returns (linear dest: seq<A>)
    requires 0 <= from as nat <= to as nat <= |source|;
    ensures source[from..to] == dest
  {
    dest := seq_alloc(to - from);
    var i:uint64 := 0;
    var count := to - from;
    while i < count
      invariant i <= count
      invariant |dest| == count as nat
      invariant forall j :: 0<=j<i ==> dest[j] == source[j + from];
    {
      dest := seq_set(dest, i, seq_get(source, i+from));
      i := i + 1;
    }
  }

  method AllocAndMoveLseq<A>(linear source: lseq<A>, from: uint64, to: uint64)
    returns (linear looted: lseq<A>, linear loot: lseq<A>)
    requires 0 <= from as nat <= to as nat <= |source|
    requires forall j :: from as nat <= j < to as nat ==> j in source
    ensures lseq_has_all(loot)
    ensures lseqs(loot) == lseqs(source)[from..to]
    ensures |looted| == |source|
    ensures forall j :: 0<=j<|source| && !(from as nat <= j < to as nat) ==> looted[j] == old(source)[j]
    ensures forall j :: 0<=j<|source|
      ==> lseq_has(looted)[j] == if from as nat <= j < to as nat then false else lseq_has(source)[j]
  {
    looted := source;
    ghost var count := (to - from) as nat;
    loot := lseq_alloc(to - from);
    var i:uint64 := from;
    assert to as nat <= |old(source)|;
    while i < to
      invariant from <= i <= to
      invariant |loot| == count
      invariant to as nat <= |looted|
      invariant forall j :: i <= j < to ==> lseq_has(looted)[j]
      invariant forall j :: 0 <= j < to-from ==> lseq_has(loot)[j] == (j < i-from)
      invariant lseqs(loot)[..i-from] == lseqs(old(source))[from..i]
      invariant |looted| == |old(source)|
      invariant forall j :: 0<=j<|source| && !(from as nat <= j < i as nat) ==> looted[j] == old(source)[j]
      invariant forall j :: 0<=j<|looted|
      ==> lseq_has(looted)[j] == if from as nat <= j < i as nat then false else lseq_has(old(source))[j]
    {
      linear var elt:A;
      looted, elt := lseq_take(looted, i);
      loot := lseq_give(loot, i-from, elt);
      i := i + 1;
    }
  }
} // module
