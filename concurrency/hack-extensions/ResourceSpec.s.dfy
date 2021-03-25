abstract module ResourceSpec {
  type R(==, !new) // TODO user can't construct/destruct the R?

  // Monoid axioms

  function unit() : R
  function add(x: R, y: R) : R

  predicate le(x: R, y: R)
  {
    exists x1 :: add(x, x1) == y
  }

  lemma add_unit(x: R)
  ensures add(x, unit()) == x

  lemma commutative(x: R, y: R)
  ensures add(x, y) == add(y, x)

  lemma associative(x: R, y: R, z: R)
  ensures add(x, add(y, z)) == add(add(x, y), z)

  predicate Init(s: R)
  predicate Update(s: R, s': R)

  predicate Valid(s: R)

  lemma valid_monotonic(x: R, y: R)
  requires Valid(add(x, y))
  ensures Valid(x)

  lemma update_monotonic(x: R, y: R, z: R)
  requires Update(x, y)
  requires Valid(add(x, z))
  ensures Update(add(x, z), add(y, z))

  function method {:extern} resources_obey_inv(shared a: R, linear b: R) : ()
  ensures Valid(add(a, b))

  function method {:extern} do_transform(
      shared a: R,
      linear b: R,
      ghost expected_out: R)
    : (linear c: R)
  requires Update(add(a, b), add(a, expected_out))
  ensures c == expected_out

  function method {:extern} get_unit() : (linear u: R)
  ensures u == unit()

  function method {:extern} get_unit_shared() : (shared u: R)
  ensures u == unit()

  function method {:extern} join(linear a: R, linear b: R) : (linear sum: R)
  ensures sum == add(a, b)

  method {:extern} split(linear sum: R, ghost a: R, ghost b: R)
  returns (linear a': R, linear b': R)
  requires sum == add(a, b)
  ensures a' == a && b' == b

  function method {:extern} join_shared(shared s: R, shared t: R, ghost expected_q: R)
    : (shared q: R)
  requires forall r :: le(s, r) && le(t, r) ==> le(expected_q, r)
  ensures expected_q == q

  function method {:extern} sub(shared s: R, ghost t: R)
   : (linear t': R)
  requires le(t, s)
  ensures t' == t

  // Refining module (.i) needs to prove these properties
  // in order to reap the benefit from the meta-properties above.

  lemma InitImpliesValid(s: R)
  requires Init(s)
  ensures Valid(s)

  lemma UpdatePreservesValid(s: R, t: R)
  requires Update(s, t)
  requires Valid(s)
  ensures Valid(t)
}
