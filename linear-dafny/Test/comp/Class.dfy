// RUN: %dafny /compile:3 /spillTargetCode:2 /compileTarget:cs "%s" > "%t"
// RUN: %dafny /compile:3 /spillTargetCode:2 /compileTarget:js "%s" >> "%t"
// RUN: %dafny /compile:3 /spillTargetCode:2 /compileTarget:go "%s" >> "%t"
// RUN: %dafny /compile:3 /spillTargetCode:2 /compileTarget:java "%s" >> "%t"
// RUN: %diff "%s.expect" "%t"

class MyClass {
  var a: int
  const b: int
  const c := 17
  static const d: int
  static const e := 18
  constructor (x: int) {
    a := 100 + x;
    b := 200 + x;
  }

  function method F(): int { 8 }
  static function method G(): int { 9 }
  method M() returns (r: int) { r := 69; }
  static method N() returns (r: int) { return 70; }
}

trait MyTrait {
  var a: int
  const b: int
  const c := 17
  static const d: int
  static const e := 18

  function method F(): int { 8 }
  static function method G(): int { 9 }
  method M() returns (r: int) { r := 69; }
  static method N() returns (r: int) { return 70; }
}

class MyTraitInstance extends MyTrait {
  constructor (x: int) {
    a := 101 + x;
    b := 201 + x;
  }

  static method SetTraitField(m : MyTrait) modifies m { m.a := N(); }
}

method CallEm(c: MyClass, t: MyTrait, i: MyTraitInstance)
  modifies c, t, i
{
  // instance fields

  print c.a, " ", t.a, " ", i.a, " ";
  c.a := c.a + 3;
  t.a := t.a + 3;
  i.a := i.a + 3;
  print c.a, " ", t.a, " ", i.a, "\n";

  // (instance and static) members via instance

  var u;

  print c.b, " ";
  print c.c, " ";
  print c.d, " ";
  print c.e, " ";
  print c.F(), " ";
  print c.G(), " ";
  u := c.M();
  print u, " ";
  u := c.N();
  print u, "\n";

  print t.b, " ";
  print t.c, " ";
  print t.d, " ";
  print t.e, " ";
  print t.F(), " ";
  print t.G(), " ";
  u := t.M();
  print u, " ";
  u := t.N();
  print u, "\n";

  print i.b, " ";
  print i.c, " ";
  print i.d, " ";
  print i.e, " ";
  print i.F(), " ";
  print i.G(), " ";
  u := i.M();
  print u, " ";
  u := i.N();
  print u, "\n";

  // static members via type name

  print MyClass.d, " ";
  print MyClass.e, " ";
  print MyClass.G(), " ";
  u := MyClass.N();
  print u, "\n";

  print MyTrait.d, " ";
  print MyTrait.e, " ";
  print MyTrait.G(), " ";
  u := MyTrait.N();
  print u, "\n";

  print MyTraitInstance.d, " ";
  print MyTraitInstance.e, " ";
  print MyTraitInstance.G(), " ";
  u := MyTraitInstance.N();
  print u, "\n";

  MyTraitInstance.SetTraitField(i);
  print i.a, "\n";
}

method Main() {
  var c := new MyClass(3);
  var t := new MyTraitInstance(2);
  var i := new MyTraitInstance(2);
  print t == t, " ", i == i, " ", i == t, "\n";
  // Upcast via local variable with rhs
  var t2 : MyTrait := t;
  // Upcast via local variable with assignment
  var t3 : MyTrait;
  t3 := t;
  // Upcast via function call
  CallEm(c, t, i);
  DependentStaticConsts.Test();
  NewtypeWithMethods.Test();
}

module Module1 {
  trait {:termination false} TraitInModule { }
}

module Module2 {
  import Module1

  class ClassExtendingTraitInOtherModule extends Module1.TraitInModule { }
}

module DependentStaticConsts {
  newtype ID = x: int | 0 <= x < 100

  // regression test: const's A,B,C,D should all be initialized before Suite is
  const A: ID := 0
  const B: ID := 1
  const C: ID := 2
  const Suite := map[A := "hello", B := "hi", C := "bye", D := "later"]
  const D: ID := 3

  method Test()
  {
    print Suite[B], " ", Suite[D], "\n";  // hi later
  }
}

newtype NewtypeWithMethods = x | 0 <= x < 42 {
  function method double() : int {
    this as int * 2
  }

  method divide(d : NewtypeWithMethods) returns (q : int, r : int) requires d != 0 {
    q := (this / d) as int;
    r := (this % d) as int;
  }

  static method Test() {
    var a : NewtypeWithMethods := 21;
    var b : NewtypeWithMethods;
    b := 4;
    var q : int;
    var r : int;
    q, r := a.divide(b);

    print a, " ", b, " ", a.double(), " ", q, " ", r, "\n";
  }
}
