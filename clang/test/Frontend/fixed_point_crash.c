// RUN: %clang_cc1 -verify -ffixed-point %s
// RUN: %clang_cc1 -verify -ffixed-point %s -fexperimental-new-constant-interpreter

union a {
  _Accum x;
  int i;
};

int fn1(void) {
  union a m;
  m.x = 5.6k;
  return m.i;
}

int fn2(void) {
  union a m;
  m.x = 7, 5.6k; // expected-warning {{expression result unused}}
  return m.x, m.i; // expected-warning {{left operand of comma operator has no effect}}
}

_Accum acc = (0.5r, 6.9k); // expected-warning {{left operand of comma operator has no effect}}
