//===-- ubsan_handlers.cc -------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Error logging entry points for the UBSan runtime.
//
//===----------------------------------------------------------------------===//

#include "ubsan_platform.h"
#if CAN_SANITIZE_UB
#include "ubsan_handlers.h"
#include "ubsan_diag.h"

#include "sanitizer_common/sanitizer_common.h"

using namespace __sanitizer;
using namespace __ubsan;

static bool ignoreReport(SourceLocation SLoc, ReportOptions Opts) {
  // If source location is already acquired, we don't need to print an error
  // report for the second time. However, if we're in an unrecoverable handler,
  // it's possible that location was required by concurrently running thread.
  // In this case, we should continue the execution to ensure that any of
  // threads will grab the report mutex and print the report before
  // crashing the program.
  return SLoc.isDisabled() && !Opts.DieAfterReport;
}

namespace __ubsan {
const char *TypeCheckKinds[] = {
    "load of", "store to", "reference binding to", "member access within",
    "member call on", "constructor call on", "downcast of", "downcast of",
    "upcast of", "cast to virtual base of"};
}

static void handleTypeMismatchImpl(TypeMismatchData *Data, ValueHandle Pointer,
                                   ReportOptions Opts) {
  Location Loc = Data->Loc.acquire();
  // Use the SourceLocation from Data to track deduplication, even if 'invalid'
  if (ignoreReport(Loc.getSourceLocation(), Opts))
    return;

  SymbolizedStackHolder FallbackLoc;
  if (Data->Loc.isInvalid()) {
    FallbackLoc.reset(getCallerLocation(Opts.pc));
    Loc = FallbackLoc;
  }

  ScopedReport R(Opts, Loc);

  if (!Pointer)
    Diag(Loc, DL_Error, "%0 null pointer of type %1")
      << TypeCheckKinds[Data->TypeCheckKind] << Data->Type;
  else if (Data->Alignment && (Pointer & (Data->Alignment - 1)))
    Diag(Loc, DL_Error, "%0 misaligned address %1 for type %3, "
                        "which requires %2 byte alignment")
      << TypeCheckKinds[Data->TypeCheckKind] << (void*)Pointer
      << Data->Alignment << Data->Type;
  else
    Diag(Loc, DL_Error, "%0 address %1 with insufficient space "
                        "for an object of type %2")
      << TypeCheckKinds[Data->TypeCheckKind] << (void*)Pointer << Data->Type;
  if (Pointer)
    Diag(Pointer, DL_Note, "pointer points here");
}

void __ubsan::__ubsan_handle_type_mismatch(TypeMismatchData *Data,
                                           ValueHandle Pointer) {
  GET_REPORT_OPTIONS(false);
  handleTypeMismatchImpl(Data, Pointer, Opts);
}
void __ubsan::__ubsan_handle_type_mismatch_abort(TypeMismatchData *Data,
                                                 ValueHandle Pointer) {
  GET_REPORT_OPTIONS(true);
  handleTypeMismatchImpl(Data, Pointer, Opts);
  Die();
}

/// \brief Common diagnostic emission for various forms of integer overflow.
template <typename T>
static void handleIntegerOverflowImpl(OverflowData *Data, ValueHandle LHS,
                                      const char *Operator, T RHS,
                                      ReportOptions Opts) {
  SourceLocation Loc = Data->Loc.acquire();
  if (ignoreReport(Loc, Opts))
    return;

  ScopedReport R(Opts, Loc);

  Diag(Loc, DL_Error, "%0 integer overflow: "
                      "%1 %2 %3 cannot be represented in type %4")
    << (Data->Type.isSignedIntegerTy() ? "signed" : "unsigned")
    << Value(Data->Type, LHS) << Operator << RHS << Data->Type;
}

#define UBSAN_OVERFLOW_HANDLER(handler_name, op, abort)                        \
  void __ubsan::handler_name(OverflowData *Data, ValueHandle LHS,              \
                             ValueHandle RHS) {                                \
    GET_REPORT_OPTIONS(abort);                                                 \
    handleIntegerOverflowImpl(Data, LHS, op, Value(Data->Type, RHS), Opts);    \
    if (abort) Die();                                                          \
  }

UBSAN_OVERFLOW_HANDLER(__ubsan_handle_add_overflow, "+", false)
UBSAN_OVERFLOW_HANDLER(__ubsan_handle_add_overflow_abort, "+", true)
UBSAN_OVERFLOW_HANDLER(__ubsan_handle_sub_overflow, "-", false)
UBSAN_OVERFLOW_HANDLER(__ubsan_handle_sub_overflow_abort, "-", true)
UBSAN_OVERFLOW_HANDLER(__ubsan_handle_mul_overflow, "*", false)
UBSAN_OVERFLOW_HANDLER(__ubsan_handle_mul_overflow_abort, "*", true)

static void handleNegateOverflowImpl(OverflowData *Data, ValueHandle OldVal,
                                     ReportOptions Opts) {
  SourceLocation Loc = Data->Loc.acquire();
  if (ignoreReport(Loc, Opts))
    return;

  ScopedReport R(Opts, Loc);

  if (Data->Type.isSignedIntegerTy())
    Diag(Loc, DL_Error,
         "negation of %0 cannot be represented in type %1; "
         "cast to an unsigned type to negate this value to itself")
      << Value(Data->Type, OldVal) << Data->Type;
  else
    Diag(Loc, DL_Error,
         "negation of %0 cannot be represented in type %1")
      << Value(Data->Type, OldVal) << Data->Type;
}

void __ubsan::__ubsan_handle_negate_overflow(OverflowData *Data,
                                             ValueHandle OldVal) {
  GET_REPORT_OPTIONS(false);
  handleNegateOverflowImpl(Data, OldVal, Opts);
}
void __ubsan::__ubsan_handle_negate_overflow_abort(OverflowData *Data,
                                                    ValueHandle OldVal) {
  GET_REPORT_OPTIONS(true);
  handleNegateOverflowImpl(Data, OldVal, Opts);
  Die();
}

static void handleDivremOverflowImpl(OverflowData *Data, ValueHandle LHS,
                                     ValueHandle RHS, ReportOptions Opts) {
  SourceLocation Loc = Data->Loc.acquire();
  if (ignoreReport(Loc, Opts))
    return;

  ScopedReport R(Opts, Loc);

  Value LHSVal(Data->Type, LHS);
  Value RHSVal(Data->Type, RHS);
  if (RHSVal.isMinusOne())
    Diag(Loc, DL_Error,
         "division of %0 by -1 cannot be represented in type %1")
      << LHSVal << Data->Type;
  else
    Diag(Loc, DL_Error, "division by zero");
}

void __ubsan::__ubsan_handle_divrem_overflow(OverflowData *Data,
                                             ValueHandle LHS, ValueHandle RHS) {
  GET_REPORT_OPTIONS(false);
  handleDivremOverflowImpl(Data, LHS, RHS, Opts);
}
void __ubsan::__ubsan_handle_divrem_overflow_abort(OverflowData *Data,
                                                    ValueHandle LHS,
                                                    ValueHandle RHS) {
  GET_REPORT_OPTIONS(true);
  handleDivremOverflowImpl(Data, LHS, RHS, Opts);
  Die();
}

static void handleShiftOutOfBoundsImpl(ShiftOutOfBoundsData *Data,
                                       ValueHandle LHS, ValueHandle RHS,
                                       ReportOptions Opts) {
  SourceLocation Loc = Data->Loc.acquire();
  if (ignoreReport(Loc, Opts))
    return;

  ScopedReport R(Opts, Loc);

  Value LHSVal(Data->LHSType, LHS);
  Value RHSVal(Data->RHSType, RHS);
  if (RHSVal.isNegative())
    Diag(Loc, DL_Error, "shift exponent %0 is negative") << RHSVal;
  else if (RHSVal.getPositiveIntValue() >= Data->LHSType.getIntegerBitWidth())
    Diag(Loc, DL_Error,
         "shift exponent %0 is too large for %1-bit type %2")
      << RHSVal << Data->LHSType.getIntegerBitWidth() << Data->LHSType;
  else if (LHSVal.isNegative())
    Diag(Loc, DL_Error, "left shift of negative value %0") << LHSVal;
  else
    Diag(Loc, DL_Error,
         "left shift of %0 by %1 places cannot be represented in type %2")
      << LHSVal << RHSVal << Data->LHSType;
}

void __ubsan::__ubsan_handle_shift_out_of_bounds(ShiftOutOfBoundsData *Data,
                                                 ValueHandle LHS,
                                                 ValueHandle RHS) {
  GET_REPORT_OPTIONS(false);
  handleShiftOutOfBoundsImpl(Data, LHS, RHS, Opts);
}
void __ubsan::__ubsan_handle_shift_out_of_bounds_abort(
                                                     ShiftOutOfBoundsData *Data,
                                                     ValueHandle LHS,
                                                     ValueHandle RHS) {
  GET_REPORT_OPTIONS(true);
  handleShiftOutOfBoundsImpl(Data, LHS, RHS, Opts);
  Die();
}

static void handleOutOfBoundsImpl(OutOfBoundsData *Data, ValueHandle Index,
                                  ReportOptions Opts) {
  SourceLocation Loc = Data->Loc.acquire();
  if (ignoreReport(Loc, Opts))
    return;

  ScopedReport R(Opts, Loc);

  Value IndexVal(Data->IndexType, Index);
  Diag(Loc, DL_Error, "index %0 out of bounds for type %1")
    << IndexVal << Data->ArrayType;
}

void __ubsan::__ubsan_handle_out_of_bounds(OutOfBoundsData *Data,
                                           ValueHandle Index) {
  GET_REPORT_OPTIONS(false);
  handleOutOfBoundsImpl(Data, Index, Opts);
}
void __ubsan::__ubsan_handle_out_of_bounds_abort(OutOfBoundsData *Data,
                                                 ValueHandle Index) {
  GET_REPORT_OPTIONS(true);
  handleOutOfBoundsImpl(Data, Index, Opts);
  Die();
}

static void handleBuiltinUnreachableImpl(UnreachableData *Data,
                                         ReportOptions Opts) {
  ScopedReport R(Opts, Data->Loc);
  Diag(Data->Loc, DL_Error, "execution reached a __builtin_unreachable() call");
}

void __ubsan::__ubsan_handle_builtin_unreachable(UnreachableData *Data) {
  GET_REPORT_OPTIONS(true);
  handleBuiltinUnreachableImpl(Data, Opts);
  Die();
}

static void handleMissingReturnImpl(UnreachableData *Data, ReportOptions Opts) {
  ScopedReport R(Opts, Data->Loc);
  Diag(Data->Loc, DL_Error,
       "execution reached the end of a value-returning function "
       "without returning a value");
}

void __ubsan::__ubsan_handle_missing_return(UnreachableData *Data) {
  GET_REPORT_OPTIONS(true);
  handleMissingReturnImpl(Data, Opts);
  Die();
}

static void handleVLABoundNotPositive(VLABoundData *Data, ValueHandle Bound,
                                      ReportOptions Opts) {
  SourceLocation Loc = Data->Loc.acquire();
  if (ignoreReport(Loc, Opts))
    return;

  ScopedReport R(Opts, Loc);

  Diag(Loc, DL_Error, "variable length array bound evaluates to "
                      "non-positive value %0")
    << Value(Data->Type, Bound);
}

void __ubsan::__ubsan_handle_vla_bound_not_positive(VLABoundData *Data,
                                                    ValueHandle Bound) {
  GET_REPORT_OPTIONS(false);
  handleVLABoundNotPositive(Data, Bound, Opts);
}
void __ubsan::__ubsan_handle_vla_bound_not_positive_abort(VLABoundData *Data,
                                                          ValueHandle Bound) {
  GET_REPORT_OPTIONS(true);
  handleVLABoundNotPositive(Data, Bound, Opts);
  Die();
}

static bool looksLikeFloatCastOverflowDataV1(void *Data) {
  // First field is either a pointer to filename or a pointer to a
  // TypeDescriptor.
  u8 *FilenameOrTypeDescriptor;
  internal_memcpy(&FilenameOrTypeDescriptor, Data,
                  sizeof(FilenameOrTypeDescriptor));

  // Heuristic: For float_cast_overflow, the TypeKind will be either TK_Integer
  // (0x0) or TK_Float (0x1). Adding both bytes will be 0 or 1 (for BE or LE).
  // If it were a filename, adding two printable characters will not yield such
  // a value.
  u16 MaybeFromTypeKind =
      FilenameOrTypeDescriptor[0] + FilenameOrTypeDescriptor[1];
  return MaybeFromTypeKind < 2;
}

static void handleFloatCastOverflow(void *DataPtr, ValueHandle From,
                                    ReportOptions Opts) {
  SymbolizedStackHolder CallerLoc;
  Location Loc;
  const TypeDescriptor *FromType, *ToType;

  if (looksLikeFloatCastOverflowDataV1(DataPtr)) {
    auto Data = reinterpret_cast<FloatCastOverflowData *>(DataPtr);
    CallerLoc.reset(getCallerLocation(Opts.pc));
    Loc = CallerLoc;
    FromType = &Data->FromType;
    ToType = &Data->ToType;
  } else {
    auto Data = reinterpret_cast<FloatCastOverflowDataV2 *>(DataPtr);
    SourceLocation SLoc = Data->Loc.acquire();
    if (ignoreReport(SLoc, Opts))
      return;
    Loc = SLoc;
    FromType = &Data->FromType;
    ToType = &Data->ToType;
  }

  ScopedReport R(Opts, Loc);

  Diag(Loc, DL_Error,
       "value %0 is outside the range of representable values of type %2")
      << Value(*FromType, From) << *FromType << *ToType;
}

void __ubsan::__ubsan_handle_float_cast_overflow(void *Data, ValueHandle From) {
  GET_REPORT_OPTIONS(false);
  handleFloatCastOverflow(Data, From, Opts);
}
void __ubsan::__ubsan_handle_float_cast_overflow_abort(void *Data,
                                                       ValueHandle From) {
  GET_REPORT_OPTIONS(true);
  handleFloatCastOverflow(Data, From, Opts);
  Die();
}

static void handleLoadInvalidValue(InvalidValueData *Data, ValueHandle Val,
                                   ReportOptions Opts) {
  SourceLocation Loc = Data->Loc.acquire();
  if (ignoreReport(Loc, Opts))
    return;

  ScopedReport R(Opts, Loc);

  Diag(Loc, DL_Error,
       "load of value %0, which is not a valid value for type %1")
    << Value(Data->Type, Val) << Data->Type;
}

void __ubsan::__ubsan_handle_load_invalid_value(InvalidValueData *Data,
                                                ValueHandle Val) {
  GET_REPORT_OPTIONS(false);
  handleLoadInvalidValue(Data, Val, Opts);
}
void __ubsan::__ubsan_handle_load_invalid_value_abort(InvalidValueData *Data,
                                                      ValueHandle Val) {
  GET_REPORT_OPTIONS(true);
  handleLoadInvalidValue(Data, Val, Opts);
  Die();
}

static void handleFunctionTypeMismatch(FunctionTypeMismatchData *Data,
                                       ValueHandle Function,
                                       ReportOptions Opts) {
  SourceLocation CallLoc = Data->Loc.acquire();
  if (ignoreReport(CallLoc, Opts))
    return;

  ScopedReport R(Opts, CallLoc);

  SymbolizedStackHolder FLoc(getSymbolizedLocation(Function));
  const char *FName = FLoc.get()->info.function;
  if (!FName)
    FName = "(unknown)";

  Diag(CallLoc, DL_Error,
       "call to function %0 through pointer to incorrect function type %1")
      << FName << Data->Type;
  Diag(FLoc, DL_Note, "%0 defined here") << FName;
}

void
__ubsan::__ubsan_handle_function_type_mismatch(FunctionTypeMismatchData *Data,
                                               ValueHandle Function) {
  GET_REPORT_OPTIONS(false);
  handleFunctionTypeMismatch(Data, Function, Opts);
}

void __ubsan::__ubsan_handle_function_type_mismatch_abort(
    FunctionTypeMismatchData *Data, ValueHandle Function) {
  GET_REPORT_OPTIONS(true);
  handleFunctionTypeMismatch(Data, Function, Opts);
  Die();
}

static void handleNonNullReturn(NonNullReturnData *Data, ReportOptions Opts) {
  SourceLocation Loc = Data->Loc.acquire();
  if (ignoreReport(Loc, Opts))
    return;

  ScopedReport R(Opts, Loc);

  Diag(Loc, DL_Error, "null pointer returned from function declared to never "
                      "return null");
  if (!Data->AttrLoc.isInvalid())
    Diag(Data->AttrLoc, DL_Note, "returns_nonnull attribute specified here");
}

void __ubsan::__ubsan_handle_nonnull_return(NonNullReturnData *Data) {
  GET_REPORT_OPTIONS(false);
  handleNonNullReturn(Data, Opts);
}

void __ubsan::__ubsan_handle_nonnull_return_abort(NonNullReturnData *Data) {
  GET_REPORT_OPTIONS(true);
  handleNonNullReturn(Data, Opts);
  Die();
}

static void handleNonNullArg(NonNullArgData *Data, ReportOptions Opts) {
  SourceLocation Loc = Data->Loc.acquire();
  if (ignoreReport(Loc, Opts))
    return;

  ScopedReport R(Opts, Loc);

  Diag(Loc, DL_Error, "null pointer passed as argument %0, which is declared to "
       "never be null") << Data->ArgIndex;
  if (!Data->AttrLoc.isInvalid())
    Diag(Data->AttrLoc, DL_Note, "nonnull attribute specified here");
}

void __ubsan::__ubsan_handle_nonnull_arg(NonNullArgData *Data) {
  GET_REPORT_OPTIONS(false);
  handleNonNullArg(Data, Opts);
}

void __ubsan::__ubsan_handle_nonnull_arg_abort(NonNullArgData *Data) {
  GET_REPORT_OPTIONS(true);
  handleNonNullArg(Data, Opts);
  Die();
}

#endif  // CAN_SANITIZE_UB
