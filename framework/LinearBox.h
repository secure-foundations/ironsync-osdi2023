#pragma once

#include "DafnyRuntime.h"

namespace LinearBox_s {

  template<typename A>
  using DestructorFunction = Tuple0 (*)(A);

  template <typename A>
  class SwapAffine {
    public:
      SwapAffine(A a) {
        this-> a = a;
      }

      A Swap(A a1) {
        A ret = this->a;
        this->a = a1;
        return ret;
      }

      A Borrow() { 
        return a;
      }
    private:
      A a;
  };

  template <typename A>
  class SwapLinear {
    public:
      SwapLinear(A a, DestructorFunction<A> d) {
        this-> a = a;
        this-> d = d;
      }

      A Swap(A a1) {
        A ret = this->a;
        this->a = a1;
        return ret;
      }

      A Borrow() { 
        return a;
      }

      ~SwapLinear() {
        d(a);
      }
    private:
      A a;
      DestructorFunction<A> d;
  };

  template<typename A>
  DestructorFunction<A> ToDestructor(Tuple0 (*f)(A)) {
    return f;
  }

}
