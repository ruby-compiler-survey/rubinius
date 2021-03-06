#include "instructions.hpp"

namespace rubinius {
  namespace instructions {
    inline void n_iinc(CF, R0, R1) {
      REG(r0) = REG(r1) + 1;
    }
  }
}
