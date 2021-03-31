include "HTResource.i.dfy"

abstract module Main {
  import ARS = HTResource
  import Ifc = MapIfc

  type MutexTable(==,!new) // using this name so the impl is more readable
  predicate Inv(o: MutexTable)

  method init(linear in_r: ARS.R)
  returns (o: MutexTable, linear out_r: ARS.R)
  requires ARS.Init(in_r)
  ensures Inv(o)

  method call(o: MutexTable, input: Ifc.Input,
      rid: int, linear in_r: ARS.R)
  returns (output: Ifc.Output, linear out_r: ARS.R)
  requires Inv(o)
  requires in_r == ARS.input_ticket(rid, input)
  ensures out_r == ARS.output_stub(rid, output)
}
