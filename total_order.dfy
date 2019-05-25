include "mathematics.dfy"
  
abstract module Total_Order {
  import Mathematics
    
	type Element(!new,==)

	predicate method lt(a: Element, b: Element)
	{
		lte(a, b) && a != b
	}
		
	predicate method lte(a: Element, b: Element)
		ensures lte(a, b) == ltedef(a, b);
		ensures ltedef(a, b) || ltedef(b, a); // Total
		ensures ltedef(a, b) && ltedef(b, a) ==> a == b; // Antisymmetric
		ensures forall a, b, c :: ltedef(a, b) && ltedef(b, c) ==> ltedef(a, c); // Transitive

	predicate method ltedef(a: Element, b: Element)

  function method Min(a: Element, b: Element) : Element
  {
    if lte(a, b) then a else b
  }
    
  method SeqMinIndex(run: seq<Element>) returns (pos: int)
    requires 0 < |run|;
    ensures 0 <= pos < |run|;
    ensures forall i :: 0 <= i < |run| ==> lte(run[pos], run[i]);
  {
    pos := 0;
    var i := 1;
    while i < |run|
      invariant 0 <= i <= |run|;
      invariant pos < i;
      invariant forall j :: 0 <= j < i ==> lte(run[pos], run[j]);
    {
      if lt(run[i], run[pos]) {
        pos := i;
      }
      i := i + 1;
    }
  }

  method SeqMin(run: seq<Element>) returns (elt: Element)
    requires 0 < |run|;
    ensures elt in run;
    ensures forall elt':: elt' in run ==> lte(elt, elt');
  {
    var index := SeqMinIndex(run);
    elt := run[index];
  }
  
  method SeqMaxIndex(run: seq<Element>) returns (pos: int)
    requires 0 < |run|;
    ensures 0 <= pos < |run|;
    ensures forall i :: 0 <= i < |run| ==> lte(run[i], run[pos]);
  {
    pos := 0;
    var i := 1;
    while i < |run|
      invariant 0 <= i <= |run|;
      invariant pos < i;
      invariant forall j :: 0 <= j < i ==> lte(run[j], run[pos]);
    {
      if lt(run[pos], run[i]) {
        pos := i;
      }
      i := i + 1;
    }
  }

  method SeqMax(run: seq<Element>) returns (elt: Element)
    requires 0 < |run|;
    ensures elt in run;
    ensures forall elt':: elt' in run ==> lte(elt', elt);
  {
    var index := SeqMaxIndex(run);
    elt := run[index];
  }
  
  function method Max(a: Element, b: Element) : Element
  {
    if lte(a, b) then b else a
  }
    
  predicate method IsSorted(run: seq<Element>) {
    forall i, j :: 0 <= i <= j < |run| ==> lte(run[i], run[j])
  }

  method FindLargestLTE(run: seq<Element>, needle: Element) returns (pos: int)
    requires IsSorted(run);
    requires 0 < |run|;
    ensures -1 <= pos < |run|;
    ensures forall i :: 0 <= i <= pos ==> lte(run[i], needle);
    ensures forall i :: pos < i < |run| ==> lt(needle, run[i]);
  {
    pos := -1;
    while pos < |run|-1 && lte(run[pos + 1], needle)
      invariant -1 <= pos < |run|;
      invariant forall i :: 0 <= i <= pos ==> lte(run[i], needle);
    {
      pos := pos + 1;
    }
  }

  method Merge(run1: seq<Element>, run2: seq<Element>) returns (result: array<Element>)
    requires 0 < |run1|;
    requires IsSorted(run1);
    requires IsSorted(run2);
    ensures multiset(result[..]) == multiset(run1) + multiset(run2);
    ensures IsSorted(result[..]);
  {
    result := new Element[|run1| + |run2|](_ => run1[0]);
    var i1 := 0;
    var i2 := 0;
    assert multiset(result[0..i1+i2]) == multiset(run1[0..i1]) + multiset(run2[0..i2]);
    while i1 < |run1| || i2 < |run2|
      invariant 0 <= i1 <= |run1|;
      invariant 0 <= i2 <= |run2|;
      invariant forall i, j :: 0 <= i < i1 + i2 && i1 <= j < |run1| ==> lte(result[i], run1[j]);
      invariant forall i, j :: 0 <= i < i1 + i2 && i2 <= j < |run2| ==> lte(result[i], run2[j]);
      invariant IsSorted(result[0..i1 + i2]);
      invariant multiset(result[0..i1+i2]) == multiset(run1[0..i1]) + multiset(run2[0..i2]);
      decreases |run1| + |run2| - i1 - i2;
    {
      if i1 < |run1| && i2 < |run2| {
        if lte(run1[i1], run2[i2]) {
          result[i1 + i2] := run1[i1];
          i1 := i1 + 1;
          assert run1[0..i1] == run1[0..i1-1] + run1[i1-1..i1]; // OBSERVE
        } else {
          result[i1 + i2] := run2[i2];
          i2 := i2 + 1;
          assert run2[0..i2] == run2[0..i2-1] + run2[i2-1..i2]; // OBSERVE
        }
      } else if i1 < |run1| {
          result[i1 + i2] := run1[i1];
          i1 := i1 + 1;
          assert run1[0..i1] == run1[0..i1-1] + run1[i1-1..i1];
      } else {
          result[i1 + i2] := run2[i2];
          i2 := i2 + 1;
          assert run2[0..i2] == run2[0..i2-1] + run2[i2-1..i2];
      }
    }
    assert result[..] == result[0..i1+i2]; // OBSERVE
    assert run1 == run1[0..i1]; // OBSERVE
    assert run2 == run2[0..i2]; // OBSERVE
  }

  method MergeSort(run: seq<Element>) returns (result: array<Element>)
    ensures multiset(result[..]) == multiset(run);
    ensures IsSorted(result[..]);
  {
    if |run| == 0 {
      result := new Element[|run|];
    } else if |run| <= 1 {
      result := new Element[|run|](i => run[0]);
    } else {
      var i := |run| / 2;
      assert |multiset(run[..i])| > 0; // OBSERVE
      assert run == run[..i] + run[i..]; // OBSERVE
      var a1 := MergeSort(run[..i]);
      var a2 := MergeSort(run[i..]);
      result :=  Merge(a1[..], a2[..]);
    }
  }

  predicate SetAllLte(a: set<Element>, b: set<Element>) {
    forall x, y :: x in a && y in b ==> lte(x, y)
  }
  
  predicate SetAllLt(a: set<Element>, b: set<Element>) {
    forall x, y :: x in a && y in b ==> lt(x, y)
  }

  lemma SetLtLteTransitivity(a: set<Element>, b: set<Element>, c: set<Element>)
    requires SetAllLt(a, b);
    requires SetAllLte(b, c);
    requires |b| > 0;
    ensures  SetAllLt(a, c);
  {
  }
  
  lemma SetAllLtImpliesDisjoint(a: set<Element>, b: set<Element>)
    requires SetAllLt(a, b);
    ensures a !! b;
  {}
}

abstract module Bounded_Total_Order refines Total_Order {
  import Base_Order : Total_Order
  datatype Element = Min_Element | Element(e: Base_Order.Element) | Max_Element

  predicate method lte(a: Element, b: Element) {
      || a.Min_Element?
      || b.Max_Element?
      || (a.Element? && b.Element? && Base_Order.lte(a.e, b.e))
  }

  predicate method ltedef(a: Element, b: Element) {
      || a.Min_Element?
      || b.Max_Element?
      || (a.Element? && b.Element? && Base_Order.lte(a.e, b.e))
  }
}

module Integer_Order refines Total_Order {
  type Element = int

  predicate method lte(a: Element, b: Element) {
    a <= b
  }

  predicate method ltedef(a: Element, b: Element) {
    a <= b
  }
}

module Char_Order refines Total_Order {
  type Element = char

  predicate method lte(a: Element, b: Element) {
    a <= b
  }

  predicate method ltedef(a: Element, b: Element) {
    a <= b
  }
}


// method Main() {
//   print Integer_Order.lt(10, 11);
//   print "\n";
//   print Integer_Order.lt(11, 10);
//   print "\n";
//   print Integer_Order.lt(11, 11);
//   print "\n";
//   print Integer_Order.lte(11, 11);
//   print "\n";
// }
