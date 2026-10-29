/// Template functions for traversal

/// Apply function object F to each level t in [0, x].
template <template <u8, typename T> typename F, u8 x, typename T>
auto Apply(T&& t) -> void {
  if constexpr (x >= 0) {
    if constexpr (x != 0) Apply<F, x - 1>(t);
    F<x, T>{}(t);
  }
}

/// Apply function object F to each level t in [0, x] in reverse order.
template <template <u8, typename T> typename F, u8 x, typename T>
auto ApplyReverse(T&& t) -> void {
  if constexpr (x >= 0) {
    F<x, T>{}(t);
    if constexpr (x != 0) ApplyReverse<F, x - 1>(t);
  }
}

/// Common calculations
// ----------------------------------------------------------------
#define SUM_U64(x) (accumulate(x.begin(), x.end(), 0ull))
#define SUM_DOUBLE(x) (accumulate(x.begin(), x.end(), 0.0))
#define AVG(x) (SUM(x) / x.size())

/// Deprecated macro definitions
// /// Current name, preceding name
// // ----------------------------------------------------------------
// #define CAT_NAME(name, x) H_CAT_NAME(name, x)
// #define H_CAT_NAME(name, x) name##x

// #define CAT_PREV_NAME(name, x) H_CAT_PREV_NAME(name, x)
// #define H_CAT_PREV_NAME(name, x) H_CAT_PREV_NAME_##x(name)
// #define H_CAT_PREV_NAME_4(name) name##3
// #define H_CAT_PREV_NAME_3(name) name##2
// #define H_CAT_PREV_NAME_2(name) name##1
// #define H_CAT_PREV_NAME_1(name) name##0

// /// Unroll the loop in order, applying function F to x
// // ----------------------------------------------------------------
// #define APPLY(F, x) H_APPLY(F, x)
// #define H_APPLY(F, x) H_APPLY_##x(F)
// #define H_APPLY_4(F) \
//   H_APPLY_3(F);      \
//   F(4)
// #define H_APPLY_3(F) \
//   H_APPLY_2(F);      \
//   F(3)
// #define H_APPLY_2(F) \
//   H_APPLY_1(F);      \
//   F(2)
// #define H_APPLY_1(F) \
//   H_APPLY_0(F);      \
//   F(1)
// #define H_APPLY_0(F) F(0)

// /// Unroll the loop in reverse order, applying function F to x
// // ----------------------------------------------------------------
// #define APPLY_REVERSE(F, x) H_APPLY_REVERSE(F, x)
// #define H_APPLY_REVERSE(F, x) H_APPLY_REVERSE_##x(F)
// #define H_APPLY_REVERSE_4(F) \
//   F(4);                      \
//   H_APPLY_REVERSE_3(F)
// #define H_APPLY_REVERSE_3(F) \
//   F(3)                       \
//   H_APPLY_REVERSE_2(F)
// #define H_APPLY_REVERSE_3(F) \
//   F(3)                       \
//   H_APPLY_REVERSE_2(F)
// #define H_APPLY_REVERSE_1(F) \
//   F(1)                       \
//   H_APPLY_REVERSE_0(F)
// #define H_APPLY_REVERSE_0(F) F(0)

// /// Unroll the loop in reverse order, applying function F to x
// // ----------------------------------------------------------------
// #define APPLY_PREV(F, x) H_APPLY_PREV(F, x)
// #define H_APPLY_PREV(F, x) H_APPLY_PREV_##x(F)
// #define H_APPLY_PREV_4(F) \
//   H_APPLY_PREV_3(F);      \
//   F(3)
// #define H_APPLY_PREV_3(F) \
//   H_APPLY_PREV_2(F);      \
//   F(2)
// #define H_APPLY_PREV_2(F) \
//   H_APPLY_PREV_1(F);      \
//   F(1)
// #define H_APPLY_PREV_1(F) \
//   H_APPLY_PREV_0(F);      \
//   F(0)
// #define H_APPLY_PREV_0(F)

// /// Generate a comma expression.
// // ----------------------------------------------------------------
// #define ITERATE_COMMA(x, PATTERN) H_ITERATE_COMMA(x, PATTERN)
// #define H_ITERATE_COMMA(x, PATTERN) H_ITERATE_COMMA_##x(PATTERN)
// #define H_ITERATE_COMMA_4(PATTERN) H_ITERATE_COMMA_3(PATTERN), PATTERN(4)
// #define H_ITERATE_COMMA_3(PATTERN) H_ITERATE_COMMA_2(PATTERN), PATTERN(3)
// #define H_ITERATE_COMMA_2(PATTERN) H_ITERATE_COMMA_1(PATTERN), PATTERN(2)
// #define H_ITERATE_COMMA_1(PATTERN) H_ITERATE_COMMA_0(PATTERN), PATTERN(1)
// #define H_ITERATE_COMMA_0(PATTERN) PATTERN(0)

// /// Repeated macro definitions
// // ----------------------------------------------------------------
// #define REPEAT_MACRO(x, s) H_REPEAT_MACRO(x, s)
// #define H_REPEAT_MACRO(x, s) H_REPEAT_MACRO_##x(s)
// #define H_REPEAT_MACRO_4(s) H_REPEAT_MACRO_3(s) s
// #define H_REPEAT_MACRO_3(s) H_REPEAT_MACRO_2(s) s
// #define H_REPEAT_MACRO_2(s) H_REPEAT_MACRO_1(s) s
// #define H_REPEAT_MACRO_1(s) H_REPEAT_MACRO_0(s) s
// #define H_REPEAT_MACRO_0(s) s