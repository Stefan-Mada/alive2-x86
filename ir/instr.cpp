// Copyright (c) 2018-present The Alive2 Authors.
// Distributed under the MIT license that can be found in the LICENSE file.

#include "ir/instr.h"
#include "ir/function.h"
#include "ir/globals.h"
#include "ir/type.h"
#include "smt/expr.h"
#include "smt/exprs.h"
#include "smt/solver.h"
#include "util/compiler.h"
#include <functional>
#include <numeric>
#include <sstream>

using namespace smt;
using namespace util;
using namespace std;

#define RAUW(val)    \
  if (val == &what)  \
    val = &with
#define DEFINE_AS_RETZERO(cls, method) \
  uint64_t cls::method() const { return 0; }
#define DEFINE_AS_RETZEROALIGN(cls, method) \
  pair<uint64_t, uint64_t> cls::method() const { return { 0, 1 }; }
#define DEFINE_AS_RETFALSE(cls, method) \
  bool cls::method() const { return false; }
#define DEFINE_AS_EMPTYACCESS(cls) \
  MemInstr::ByteAccessInfo cls::getByteAccessInfo() const \
  { return {}; }

// log2 of max number of var args per function
#define VARARG_BITS 8

namespace {
struct print_type {
  IR::Type &ty;
  const char *pre, *post;

  print_type(IR::Type &ty, const char *pre = "", const char *post = " ")
    : ty(ty), pre(pre), post(post) {}

  friend ostream& operator<<(ostream &os, const print_type &s) {
    auto str = s.ty.toString();
    return str.empty() ? os : (os << s.pre << str << s.post);
  }
};

struct LoopLikeFunctionApproximator {
  // input: (i, is the last iter?)
  // output: (value, nonpoison, UB, continue?)
  using fn_t = function<tuple<expr, expr, AndExpr, expr>(unsigned, bool)>;
  fn_t ith_exec;

  LoopLikeFunctionApproximator(fn_t ith_exec) : ith_exec(std::move(ith_exec)) {}

  // (value, nonpoison, UB)
  tuple<expr, expr, expr> encode(IR::State &s, unsigned unroll_cnt) {
    AndExpr prefix;
    return _loop(s, prefix, 0, unroll_cnt);
  }

  // (value, nonpoison, UB)
  tuple<expr, expr, expr> _loop(IR::State &s, AndExpr &prefix, unsigned i,
                                unsigned unroll_cnt) {
    bool is_last = i >= unroll_cnt - 1;
    auto [res_i, np_i, ub_i, continue_i] = ith_exec(i, is_last);
    auto ub = ub_i();
    prefix.add(ub_i);

    // Keep going if the function is being applied to a constant input
    is_last &= !continue_i.isConst();

    if (is_last)
      s.addPre(prefix().implies(!continue_i));

    if (is_last || continue_i.isFalse() || ub.isFalse() || !s.isViablePath())
      return { std::move(res_i), std::move(np_i), std::move(ub) };

    prefix.add(continue_i);
    auto [val_next, np_next, ub_next] = _loop(s, prefix, i + 1, unroll_cnt);
    return { expr::mkIf(continue_i, std::move(val_next), std::move(res_i)),
             np_i && continue_i.implies(np_next),
             ub && continue_i.implies(ub_next) };
  }
};

uint64_t getGlobalVarSize(const IR::Value *V) {
  if (auto *V2 = isNoOp(*V))
    return getGlobalVarSize(V2);
  if (auto glb = dynamic_cast<const IR::GlobalVariable *>(V))
    return glb->size();
  return UINT64_MAX;
}

}


namespace IR {

bool Instr::propagatesPoison() const {
  // be on the safe side
  return false;
}

expr Instr::getTypeConstraints() const {
  UNREACHABLE();
  return {};
}


BinOp::BinOp(Type &type, string &&name, Value &lhs, Value &rhs, Op op,
             unsigned flags)
  : Instr(type, std::move(name)), lhs(&lhs), rhs(&rhs), op(op), flags(flags) {
  switch (op) {
  case Add:
  case Sub:
  case Mul:
  case Shl:
    assert((flags & (NSW | NUW)) == flags);
    break;
  case SDiv:
  case UDiv:
  case AShr:
  case LShr:
    assert((flags & Exact) == flags);
    break;
  default:
    assert(flags == None);
    break;
  }
}

vector<Value*> BinOp::operands() const {
  return { lhs, rhs };
}

bool BinOp::propagatesPoison() const {
  return true;
}

void BinOp::rauw(const Value &what, Value &with) {
  RAUW(lhs);
  RAUW(rhs);
}

void BinOp::print(ostream &os) const {
  const char *str = nullptr;
  switch (op) {
  case Add:           str = "add "; break;
  case Sub:           str = "sub "; break;
  case Mul:           str = "mul "; break;
  case SDiv:          str = "sdiv "; break;
  case UDiv:          str = "udiv "; break;
  case SRem:          str = "srem "; break;
  case URem:          str = "urem "; break;
  case Shl:           str = "shl "; break;
  case AShr:          str = "ashr "; break;
  case LShr:          str = "lshr "; break;
  case SAdd_Sat:      str = "sadd_sat "; break;
  case UAdd_Sat:      str = "uadd_sat "; break;
  case SSub_Sat:      str = "ssub_sat "; break;
  case USub_Sat:      str = "usub_sat "; break;
  case SShl_Sat:      str = "sshl_sat "; break;
  case UShl_Sat:      str = "ushl_sat "; break;
  case And:           str = "and "; break;
  case Or:            str = "or "; break;
  case Xor:           str = "xor "; break;
  case Cttz:          str = "cttz "; break;
  case Ctlz:          str = "ctlz "; break;
  case SAdd_Overflow: str = "sadd_overflow "; break;
  case UAdd_Overflow: str = "uadd_overflow "; break;
  case SSub_Overflow: str = "ssub_overflow "; break;
  case USub_Overflow: str = "usub_overflow "; break;
  case SMul_Overflow: str = "smul_overflow "; break;
  case UMul_Overflow: str = "umul_overflow "; break;
  case UMin:          str = "umin "; break;
  case UMax:          str = "umax "; break;
  case SMin:          str = "smin "; break;
  case SMax:          str = "smax "; break;
  case Abs:           str = "abs "; break;
  }

  os << getName() << " = " << str;

  if (flags & NSW)
    os << "nsw ";
  if (flags & NUW)
    os << "nuw ";
  if (flags & Exact)
    os << "exact ";
  os << *lhs << ", " << rhs->getName();
}

static void div_ub(State &s, const expr &a, const expr &b, const expr &ap,
                   const expr &bp, bool sign) {
  // addUB(bp) is not needed because it is registered by getAndAddPoisonUB.
  assert(!bp.isValid() || bp.isTrue());
  s.addUB(b != 0);
  if (sign)
    s.addUB((ap && a != expr::IntSMin(b.bits())) || b != expr::mkInt(-1, b));
}

StateValue BinOp::toSMT(State &s) const {
  bool vertical_zip = false;
  function<StateValue(const expr&, const expr&, const expr&, const expr&)>
    fn, scalar_op;

  switch (op) {
  case Add:
    fn = [&](auto &a, auto &ap, auto &b, auto &bp) -> StateValue {
      expr non_poison = true;
      if (flags & NSW)
        non_poison &= a.add_no_soverflow(b);
      if (flags & NUW)
        non_poison &= a.add_no_uoverflow(b);
      return { a + b, std::move(non_poison) };
    };
    break;

  case Sub:
    fn = [&](auto &a, auto &ap, auto &b, auto &bp) -> StateValue {
      expr non_poison = true;
      if (flags & NSW)
        non_poison &= a.sub_no_soverflow(b);
      if (flags & NUW)
        non_poison &= a.sub_no_uoverflow(b);
      return { a - b, std::move(non_poison) };
    };
    break;

  case Mul:
    fn = [&](auto &a, auto &ap, auto &b, auto &bp) -> StateValue {
      expr non_poison = true;
      if (flags & NSW)
        non_poison &= a.mul_no_soverflow(b);
      if (flags & NUW)
        non_poison &= a.mul_no_uoverflow(b);
      return { a * b, std::move(non_poison) };
    };
    break;

  case SDiv:
    fn = [&](auto &a, auto &ap, auto &b, auto &bp) -> StateValue {
      expr non_poison = true;
      div_ub(s, a, b, ap, bp, true);
      if (flags & Exact)
        non_poison = a.sdiv_exact(b);
      return { a.sdiv(b), std::move(non_poison) };
    };
    break;

  case UDiv:
    fn = [&](auto &a, auto &ap, auto &b, auto &bp) -> StateValue {
      expr non_poison = true;
      div_ub(s, a, b, ap, bp, false);
      if (flags & Exact)
        non_poison &= a.udiv_exact(b);
      return { a.udiv(b), std::move(non_poison) };
    };
    break;

  case SRem:
    fn = [&](auto &a, auto &ap, auto &b, auto &bp) -> StateValue {
      div_ub(s, a, b, ap, bp, true);
      return { a.srem(b), true };
    };
    break;

  case URem:
    fn = [&](auto &a, auto &ap, auto &b, auto &bp) -> StateValue {
      div_ub(s, a, b, ap, bp, false);
      return { a.urem(b), true };
    };
    break;

  case Shl:
    fn = [&](auto &a, auto &ap, auto &b, auto &bp) -> StateValue {
      auto non_poison = b.ult(b.bits());
      if (flags & NSW)
        non_poison &= a.shl_no_soverflow(b);
      if (flags & NUW)
        non_poison &= a.shl_no_uoverflow(b);

      return { a << b, std::move(non_poison) };
    };
    break;

  case AShr:
    fn = [&](auto &a, auto &ap, auto &b, auto &bp) -> StateValue {
      auto non_poison = b.ult(b.bits());
      if (flags & Exact)
        non_poison &= a.ashr_exact(b);
      return { a.ashr(b), std::move(non_poison) };
    };
    break;

  case LShr:
    fn = [&](auto &a, auto &ap, auto &b, auto &bp) -> StateValue {
      auto non_poison = b.ult(b.bits());
      if (flags & Exact)
        non_poison &= a.lshr_exact(b);
      return { a.lshr(b), std::move(non_poison) };
    };
    break;

  case SAdd_Sat:
    fn = [](auto &a, auto &ap, auto &b, auto &bp) -> StateValue {
      return { a.sadd_sat(b), true };
    };
    break;

  case UAdd_Sat:
    fn = [](auto &a, auto &ap, auto &b, auto &bp) -> StateValue {
      return { a.uadd_sat(b), true };
    };
    break;

  case SSub_Sat:
    fn = [](auto &a, auto &ap, auto &b, auto &bp) -> StateValue {
      return { a.ssub_sat(b), true };
    };
    break;

  case USub_Sat:
    fn = [](auto &a, auto &ap, auto &b, auto &bp) -> StateValue {
      return { a.usub_sat(b), true };
    };
    break;

  case SShl_Sat:
    fn = [](auto &a, auto &ap, auto &b, auto &bp) -> StateValue {
      return {a.sshl_sat(b), b.ult(b.bits())};
    };
    break;

  case UShl_Sat:
    fn = [](auto &a, auto &ap, auto &b, auto &bp) -> StateValue {
      return {a.ushl_sat(b), b.ult(b.bits())};
    };
    break;

  case And:
    fn = [](auto &a, auto &ap, auto &b, auto &bp) -> StateValue {
      return { a & b, true };
    };
    break;

  case Or:
    fn = [](auto &a, auto &ap, auto &b, auto &bp) -> StateValue {
      return { a | b, true };
    };
    break;

  case Xor:
    fn = [](auto &a, auto &ap, auto &b, auto &bp) -> StateValue {
      return { a ^ b, true };
    };
    break;

  case Cttz:
    fn = [](auto &a, auto &ap, auto &b, auto &bp) -> StateValue {
      return { a.cttz(expr::mkUInt(a.bits(), a)),
               b == 0u || a != 0u };
    };
    break;

  case Ctlz:
    fn = [](auto &a, auto &ap, auto &b, auto &bp) -> StateValue {
      return { a.ctlz(),
               b == 0u || a != 0u };
    };
    break;

  case SAdd_Overflow:
    vertical_zip = true;
    fn = [](auto &a, auto &ap, auto &b, auto &bp) -> StateValue {
      return { a + b, (!a.add_no_soverflow(b)).toBVBool() };
    };
    break;

  case UAdd_Overflow:
    vertical_zip = true;
    fn = [](auto &a, auto &ap, auto &b, auto &bp) -> StateValue {
      return { a + b, (!a.add_no_uoverflow(b)).toBVBool() };
    };
    break;

  case SSub_Overflow:
    vertical_zip = true;
    fn = [](auto &a, auto &ap, auto &b, auto &bp) -> StateValue {
      return { a - b, (!a.sub_no_soverflow(b)).toBVBool() };
    };
    break;

  case USub_Overflow:
    vertical_zip = true;
    fn = [](auto &a, auto &ap, auto &b, auto &bp) -> StateValue {
      return { a - b, (!a.sub_no_uoverflow(b)).toBVBool() };
    };
    break;

  case SMul_Overflow:
    vertical_zip = true;
    fn = [](auto &a, auto &ap, auto &b, auto &bp) -> StateValue {
      return { a * b, (!a.mul_no_soverflow(b)).toBVBool() };
    };
    break;

  case UMul_Overflow:
    vertical_zip = true;
    fn = [](auto &a, auto &ap, auto &b, auto &bp) -> StateValue {
      return { a * b, (!a.mul_no_uoverflow(b)).toBVBool() };
    };
    break;

  case UMin:
  case UMax:
  case SMin:
  case SMax:
    fn = [&](auto &a, auto &ap, auto &b, auto &bp) -> StateValue {
      expr v;
      switch (op) {
      case UMin:
        v = a.umin(b);
        break;
      case UMax:
        v = a.umax(b);
        break;
      case SMin:
        v = a.smin(b);
        break;
      case SMax:
        v = a.smax(b);
        break;
      default:
        UNREACHABLE();
      }
      return { std::move(v), ap && bp };
    };
    break;

  case Abs:
    fn = [&](auto &a, auto &ap, auto &b, auto &bp) -> StateValue {
      return { a.abs(), ap && bp && (b == 0 || a != expr::IntSMin(a.bits())) };
    };
    break;
  }

  function<pair<StateValue,StateValue>(const expr&, const expr&, const expr&,
                                       const expr&)> zip_op;
  if (vertical_zip) {
    zip_op = [&](auto &a, auto &ap, auto &b, auto &bp) {
      auto [v1, v2] = fn(a, ap, b, bp);
      expr non_poison = ap && bp;
      StateValue sv1(std::move(v1), expr(non_poison));
      return make_pair(std::move(sv1), StateValue(std::move(v2), std::move(non_poison)));
    };
  } else {
    scalar_op = [&](auto &a, auto &ap, auto &b, auto &bp) -> StateValue {
      auto [v, np] = fn(a, ap, b, bp);
      return { std::move(v), ap && bp && np };
    };
  }

  auto &a = s[*lhs];
  auto &b = isDivOrRem() ? s.getAndAddPoisonUB(*rhs) : s[*rhs];

  if (lhs->getType().isVectorType()) {
    auto retty = getType().getAsAggregateType();
    vector<StateValue> vals;

    if (vertical_zip) {
      auto ty = lhs->getType().getAsAggregateType();
      vector<StateValue> vals1, vals2;
      unsigned val2idx = 1 + retty->isPadding(1);
      auto val1ty = retty->getChild(0).getAsAggregateType();
      auto val2ty = retty->getChild(val2idx).getAsAggregateType();

      for (unsigned i = 0, e = ty->numElementsConst(); i != e; ++i) {
        auto ai = ty->extract(a, i);
        auto bi = ty->extract(b, i);
        auto [v1, v2] = zip_op(ai.value, ai.non_poison, bi.value,
                               bi.non_poison);
        vals1.emplace_back(std::move(v1));
        vals2.emplace_back(std::move(v2));
      }
      vals.emplace_back(val1ty->aggregateVals(vals1));
      vals.emplace_back(val2ty->aggregateVals(vals2));
    } else {
      StateValue tmp;
      for (unsigned i = 0, e = retty->numElementsConst(); i != e; ++i) {
        auto ai = retty->extract(a, i);
        const StateValue *bi;
        switch (op) {
        case Abs:
        case Cttz:
        case Ctlz:
          bi = &b;
          break;
        default:
          tmp = retty->extract(b, i);
          bi = &tmp;
          break;
        }
        vals.emplace_back(scalar_op(ai.value, ai.non_poison, bi->value,
                                    bi->non_poison));
      }
    }
    return retty->aggregateVals(vals);
  }

  if (vertical_zip) {
    vector<StateValue> vals;
    auto [v1, v2] = zip_op(a.value, a.non_poison, b.value, b.non_poison);
    vals.emplace_back(std::move(v1));
    vals.emplace_back(std::move(v2));
    return getType().getAsAggregateType()->aggregateVals(vals);
  }
  return scalar_op(a.value, a.non_poison, b.value, b.non_poison);
}

expr BinOp::getTypeConstraints(const Function &f) const {
  expr instrconstr;
  switch (op) {
  case SAdd_Overflow:
  case UAdd_Overflow:
  case SSub_Overflow:
  case USub_Overflow:
  case SMul_Overflow:
  case UMul_Overflow:
    instrconstr = getType().enforceStructType() &&
                  lhs->getType().enforceIntOrVectorType() &&
                  lhs->getType() == rhs->getType();

    if (auto ty = getType().getAsStructType()) {
      unsigned v2idx = 1 + ty->isPadding(1);
      instrconstr &= ty->numElementsExcludingPadding() == 2 &&
                     ty->getChild(0) == lhs->getType() &&
                     ty->getChild(v2idx).enforceIntOrVectorType(1) &&
                     ty->getChild(v2idx).enforceVectorTypeEquiv(lhs->getType());
    }
    break;
  case Cttz:
  case Ctlz:
  case Abs:
    instrconstr = getType().enforceIntOrVectorType() &&
                  getType() == lhs->getType() &&
                  rhs->getType().enforceIntType(1);
    break;
  default:
    instrconstr = getType().enforceIntOrVectorType() &&
                  getType() == lhs->getType() &&
                  getType() == rhs->getType();
    break;
  }
  return Value::getTypeConstraints() && std::move(instrconstr);
}

unique_ptr<Instr> BinOp::dup(Function &f, const string &suffix) const {
  return make_unique<BinOp>(getType(), getName()+suffix, *lhs, *rhs, op, flags);
}

bool BinOp::isDivOrRem() const {
  switch (op) {
  case Op::SDiv:
  case Op::SRem:
  case Op::UDiv:
  case Op::URem:
    return true;
  default:
    return false;
  }
}


vector<Value*> FpBinOp::operands() const {
  return { lhs, rhs };
}

bool FpBinOp::propagatesPoison() const {
  return true;
}

void FpBinOp::rauw(const Value &what, Value &with) {
  RAUW(lhs);
  RAUW(rhs);
}

void FpBinOp::print(ostream &os) const {
  const char *str = nullptr;
  switch (op) {
  case FAdd:     str = "fadd "; break;
  case FSub:     str = "fsub "; break;
  case FMul:     str = "fmul "; break;
  case FDiv:     str = "fdiv "; break;
  case FRem:     str = "frem "; break;
  case FMax:     str = "fmax "; break;
  case FMin:     str = "fmin "; break;
  case FMaximum: str = "fmaximum "; break;
  case FMinimum: str = "fminimum "; break;
  case CopySign: str = "copysign "; break;
  }
  os << getName() << " = " << str << fmath << *lhs << ", " << rhs->getName();
  if (!rm.isDefault())
    os << ", rounding=" << rm;
  os << ", exceptions=" << ex;
}

static expr any_fp_zero(State &s, const expr &v) {
  expr is_zero = v.isFPZero();
  if (is_zero.isFalse())
    return v;

  // any-fp-zero2(any-fp-zero1(x)) -> any-fp-zero2(x)
  {
    expr cond, neg, negv, val;
    if (v.isIf(cond, neg, val) && neg.isFPNeg(negv) && negv.eq(val)) {
      expr a, b;
      if (cond.isAnd(a, b) && a.isVar() && a.fn_name().starts_with("anyzero") &&
          b.isIsFPZero())
        return any_fp_zero(s, val);
    }
  }

  expr var = expr::mkFreshVar("anyzero", true);
  s.addQuantVar(var);
  return expr::mkIf(var && is_zero, v.fneg(), v);
}

static expr handle_subnormal(FPDenormalAttrs::Type attr, expr &&v) {
  switch (attr) {
  case FPDenormalAttrs::IEEE:
    break;
  case FPDenormalAttrs::PositiveZero:
    v = expr::mkIf(v.isFPSubNormal(), expr::mkNumber("0", v), v);
    break;
  case FPDenormalAttrs::PreserveSign:
    v = expr::mkIf(v.isFPSubNormal(),
                   expr::mkIf(v.isFPNegative(),
                              expr::mkNumber("-0", v),
                              expr::mkNumber("0", v)),
                   v);
    break;
  }
  return std::move(v);
}

static StateValue fm_poison(State &s, const expr &a, const expr &ap,
                            const expr &b, const expr &bp, const expr &c,
                            const expr &cp,
                            function<expr(expr&,expr&,expr&)> fn,
                            const Type &ty, FastMathFlags fmath,
                            bool only_input = false,
                            bool flush_denormal = true,
                            int nary = 3) {
  expr new_a, new_b, new_c;
  if (fmath.flags & FastMathFlags::NSZ) {
    new_a = any_fp_zero(s, a);
    if (nary >= 2) {
      new_b = any_fp_zero(s, b);
      if (nary == 3)
        new_c = any_fp_zero(s, c);
    }
  } else {
    new_a = a;
    new_b = b;
    new_c = c;
  }

  if (flush_denormal) {
    auto fpdenormal = s.getFn().getFnAttrs().getFPDenormal(ty).input;
    new_a = handle_subnormal(fpdenormal, std::move(new_a));
    if (nary >= 2)
      new_b = handle_subnormal(fpdenormal, std::move(new_b));
    if (nary >= 3)
      new_c = handle_subnormal(fpdenormal, std::move(new_c));
  }

  expr val = fn(new_a, new_b, new_c);
  AndExpr non_poison;
  non_poison.add(ap);
  if (nary >= 2)
    non_poison.add(bp);
  if (nary >= 3)
    non_poison.add(cp);

  if (fmath.flags & FastMathFlags::NNaN) {
    non_poison.add(!a.isNaN());
    if (nary >= 2) {
      non_poison.add(!b.isNaN());
      if (nary == 3)
        non_poison.add(!c.isNaN());
    }
    if (!only_input)
      non_poison.add(!val.isNaN());
  }
  if (fmath.flags & FastMathFlags::NInf) {
    non_poison.add(!a.isInf());
    if (nary >= 2) {
      non_poison.add(!b.isInf());
      if (nary == 3)
        non_poison.add(!c.isInf());
    }
    if (!only_input)
      non_poison.add(!val.isInf());
  }
  if (fmath.flags & FastMathFlags::ARCP) {
    val = expr::mkUF("arcp", { val }, val);
    s.doesApproximation("arcp", val);
  }
  if (fmath.flags & FastMathFlags::Contract) {
    val = expr::mkUF("contract", { val }, val);
    s.doesApproximation("contract", val);
  }
  if (fmath.flags & FastMathFlags::Reassoc) {
    val = expr::mkUF("reassoc", { val }, val);
    s.doesApproximation("reassoc", val);
  }
  if (fmath.flags & FastMathFlags::AFN) {
    val = expr::mkUF("afn", { val }, val);
    s.doesApproximation("afn", val);
  }
  if (fmath.flags & FastMathFlags::NSZ && !only_input)
    val = any_fp_zero(s, std::move(val));

  return { std::move(val), non_poison() };
}

static StateValue fm_poison(State &s, const expr &a, const expr &ap,
                            const expr &b, const expr &bp,
                            function<expr(expr&,expr&)> fn,
                            const Type &ty, FastMathFlags fmath,
                            bool only_input = false,
                            bool flush_denormal = true) {
  return fm_poison(s, a, ap, std::move(b), bp, expr(), expr(),
                   [&](expr &a, expr &b, expr &c) { return fn(a, b); },
                   ty, fmath, only_input, flush_denormal, 2);
}

static StateValue fm_poison(State &s, const expr &a, const expr &ap,
                            function<expr(expr&)> fn, const Type &ty,
                            FastMathFlags fmath, bool only_input = false,
                            bool flush_denormal = true) {
  return fm_poison(s, a, ap, expr(), expr(), expr(), expr(),
                   [&](expr &a, expr &b, expr &c) { return fn(a); },
                   ty, fmath, only_input, flush_denormal, 1);
}

static StateValue round_value_(const function<StateValue(FpRoundingMode)> &fn,
                               const State &s, FpRoundingMode rm) {
  if (rm.isDefault())
    return fn(FpRoundingMode::RNE);

  auto &var = s.getFpRoundingMode();
  if (!rm.isDynamic()) {
    auto [v, np] = fn(rm);
    return { std::move(v), np && var == rm.getMode() };
  }

  return StateValue::mkIf(var == FpRoundingMode::RNE, fn(FpRoundingMode::RNE),
         StateValue::mkIf(var == FpRoundingMode::RNA, fn(FpRoundingMode::RNA),
         StateValue::mkIf(var == FpRoundingMode::RTP, fn(FpRoundingMode::RTP),
         StateValue::mkIf(var == FpRoundingMode::RTN, fn(FpRoundingMode::RTN),
                          fn(FpRoundingMode::RTZ)))));
}

static StateValue round_value(const function<StateValue(FpRoundingMode)> &fn,
                              const State &s, const Type &ty,
                              FpRoundingMode rm,
                              bool enable_subnormal_flush = true) {
  auto [v, np] = round_value_(fn, s, rm);
  if (enable_subnormal_flush)
    v = handle_subnormal(s.getFn().getFnAttrs().getFPDenormal(ty).output,
                         std::move(v));
  return { std::move(v), std::move(np) };
}

StateValue FpBinOp::toSMT(State &s) const {
  function<expr(const expr&, const expr&, FpRoundingMode)> fn;
  bool flush_denormal = true;

  switch (op) {
  case FAdd:
    fn = [](const expr &a, const expr &b, FpRoundingMode rm) {
      return a.fadd(b, rm.toSMT());
    };
    break;

  case FSub:
    fn = [](const expr &a, const expr &b, FpRoundingMode rm) {
      return a.fsub(b, rm.toSMT());
    };
    break;

  case FMul:
    fn = [](const expr &a, const expr &b, FpRoundingMode rm) {
      return a.fmul(b, rm.toSMT());
    };
    break;

  case FDiv:
    fn = [](const expr &a, const expr &b, FpRoundingMode rm) {
      return a.fdiv(b, rm.toSMT());
    };
    break;

  case FRem:
    fn = [&](const expr &a, const expr &b, FpRoundingMode rm) {
      // TODO; Z3 has no support for LLVM's frem which is actually an fmod
      auto val = expr::mkUF("fmod", {a, b}, a);
      s.doesApproximation("frem", val);
      return val;
    };
    break;

  case FMin:
  case FMax:
    fn = [&](const expr &a, const expr &b, FpRoundingMode rm) {
      expr ndet = expr::mkFreshVar("maxminnondet", true);
      s.addQuantVar(ndet);
      auto ndz = expr::mkIf(ndet, expr::mkNumber("0", a),
                            expr::mkNumber("-0", a));

      expr z = a.isFPZero() && b.isFPZero();
      expr cmp = op == FMin ? a.fole(b) : a.foge(b);
      return expr::mkIf(a.isNaN(), b,
                        expr::mkIf(b.isNaN(), a,
                                   expr::mkIf(z, ndz,
                                              expr::mkIf(cmp, a, b))));
    };
    break;

  case FMinimum:
  case FMaximum:
    fn = [&](const expr &a, const expr &b, FpRoundingMode rm) {
      expr zpos = expr::mkNumber("0", a), zneg = expr::mkNumber("-0", a);
      expr cmp = (op == FMinimum) ? a.fole(b) : a.foge(b);
      expr neg_cond = op == FMinimum ? (a.isFPNegative() || b.isFPNegative())
                                     : (a.isFPNegative() && b.isFPNegative());
      expr e = expr::mkIf(a.isFPZero() && b.isFPZero(),
                          expr::mkIf(neg_cond, zneg, zpos),
                          expr::mkIf(cmp, a, b));

      return expr::mkIf(a.isNaN(), a, expr::mkIf(b.isNaN(), b, e));
    };
    break;
  case CopySign:
    flush_denormal = false;
    fn = [](const expr &a, const expr &b, FpRoundingMode rm) {
      return expr::mkIf(a.isFPNegative() == b.isFPNegative(), a, a.fneg());
    };
    break;
  }

  auto scalar = [&](const auto &a, const auto &b, const Type &ty) {
    return round_value([&](auto rm) {
      return fm_poison(s, a.value, a.non_poison, b.value, b.non_poison,
                       [&](expr &a, expr &b){ return fn(a, b, rm); }, ty,
                       fmath, !flush_denormal, flush_denormal);
    }, s, ty, rm, flush_denormal);
  };

  auto &a = s[*lhs];
  auto &b = s[*rhs];

  if (lhs->getType().isVectorType()) {
    auto retty = getType().getAsAggregateType();
    vector<StateValue> vals;
    for (unsigned i = 0, e = retty->numElementsConst(); i != e; ++i) {
      vals.emplace_back(scalar(retty->extract(a, i), retty->extract(b, i),
                               retty->getChild(i)));
    }
    return retty->aggregateVals(vals);
  }
  return scalar(a, b, getType());
}

expr FpBinOp::getTypeConstraints(const Function &f) const {
  return Value::getTypeConstraints() &&
         getType().enforceFloatOrVectorType() &&
         getType() == lhs->getType() &&
         getType() == rhs->getType();
}

unique_ptr<Instr> FpBinOp::dup(Function &f, const string &suffix) const {
  return make_unique<FpBinOp>(getType(), getName()+suffix, *lhs, *rhs, op,
                              fmath);
}


vector<Value*> UnaryOp::operands() const {
  return { val };
}

bool UnaryOp::propagatesPoison() const {
  return true;
}

void UnaryOp::rauw(const Value &what, Value &with) {
  RAUW(val);

  if (auto *agg = dynamic_cast<AggregateValue*>(val))
    agg->rauw(what, with);
}

void UnaryOp::print(ostream &os) const {
  const char *str = nullptr;
  switch (op) {
  case Copy:        str = ""; break;
  case BitReverse:  str = "bitreverse "; break;
  case BSwap:       str = "bswap "; break;
  case Ctpop:       str = "ctpop "; break;
  case IsConstant:  str = "is.constant "; break;
  case FFS:         str = "ffs "; break;
  }

  os << getName() << " = " << str << *val;
}

StateValue UnaryOp::toSMT(State &s) const {
  function<StateValue(const expr&, const expr&)> fn;

  switch (op) {
  case Copy:
    if (dynamic_cast<AggregateValue *>(val))
      // Aggregate value is not registered at state.
      return val->toSMT(s);
    return s[*val];
  case BitReverse:
    fn = [](auto &v, auto np) -> StateValue {
      return { v.bitreverse(), expr(np) };
    };
    break;
  case BSwap:
    fn = [](auto &v, auto np) -> StateValue {
      return { v.bswap(), expr(np) };
    };
    break;
  case Ctpop:
    fn = [](auto &v, auto np) -> StateValue {
      return { v.ctpop(), expr(np) };
    };
    break;
  case IsConstant: {
    expr one = expr::mkUInt(1, 1);
    if (dynamic_cast<Constant *>(val))
      return { std::move(one), true };

    // may or may not be a constant
    expr var = expr::mkFreshVar("is.const", one);
    s.addQuantVar(var);
    return { std::move(var), true };
  }
  case FFS:
    fn = [](auto &v, auto np) -> StateValue {
      return { v.cttz(expr::mkInt(-1, v)) + expr::mkUInt(1, v), expr(np) };
    };
    break;
  }

  auto &v = s[*val];

  if (getType().isVectorType()) {
    vector<StateValue> vals;
    auto ty = val->getType().getAsAggregateType();
    for (unsigned i = 0, e = ty->numElementsConst(); i != e; ++i) {
      auto vi = ty->extract(v, i);
      vals.emplace_back(fn(vi.value, vi.non_poison));
    }
    return getType().getAsAggregateType()->aggregateVals(vals);
  }
  return fn(v.value, v.non_poison);
}

expr UnaryOp::getTypeConstraints(const Function &f) const {
  expr instrconstr = getType() == val->getType();
  switch (op) {
  case Copy:
    break;
  case BSwap:
    instrconstr &= getType().enforceScalarOrVectorType([](auto &scalar) {
                     return scalar.enforceIntType() &&
                            scalar.sizeVar().urem(expr::mkUInt(16, 8)) == 0;
                   });
    break;
  case BitReverse:
  case Ctpop:
  case FFS:
    instrconstr &= getType().enforceIntOrVectorType();
    break;
  case IsConstant:
    instrconstr = getType().enforceIntType(1);
    break;
  }

  return Value::getTypeConstraints() && std::move(instrconstr);
}

static Value* dup_aggregate(Function &f, Value *val) {
  if (auto *agg = dynamic_cast<AggregateValue*>(val)) {
    vector<Value*> elems;
    for (auto v : agg->getVals()) {
      elems.emplace_back(dup_aggregate(f, v));
    }
    auto agg_new
      = make_unique<AggregateValue>(agg->getType(), std::move(elems));
    auto ret = agg_new.get();
    f.addAggregate(std::move(agg_new));
    return ret;
  }
  return val;
}

unique_ptr<Instr> UnaryOp::dup(Function &f, const string &suffix) const {
  auto *newval = val;
  if (dynamic_cast<AggregateValue*>(val) != nullptr && op == Copy)
    newval = dup_aggregate(f, val);

  return make_unique<UnaryOp>(getType(), getName() + suffix, *newval, op);
}


vector<Value*> FpUnaryOp::operands() const {
  return { val };
}

bool FpUnaryOp::propagatesPoison() const {
  return true;
}

void FpUnaryOp::rauw(const Value &what, Value &with) {
  RAUW(val);
}

void FpUnaryOp::print(ostream &os) const {
  const char *str = nullptr;
  switch (op) {
  case FAbs:      str = "fabs "; break;
  case FNeg:      str = "fneg "; break;
  case Ceil:      str = "ceil "; break;
  case Floor:     str = "floor "; break;
  case RInt:      str = "rint "; break;
  case NearbyInt: str = "nearbyint "; break;
  case Round:     str = "round "; break;
  case RoundEven: str = "roundeven "; break;
  case Trunc:     str = "trunc "; break;
  case Sqrt:      str = "sqrt "; break;
  }

  os << getName() << " = " << str << fmath << *val;
  if (!rm.isDefault())
    os << ", rounding=" << rm;
  os << ", exceptions=" << ex;
}

StateValue FpUnaryOp::toSMT(State &s) const {
  expr (*fn)(const expr&, FpRoundingMode);
  bool flush_denormal = true;

  switch (op) {
  case FAbs:
    flush_denormal = false;
    fn = [](const expr &v, FpRoundingMode rm) { return v.fabs(); };
    break;
  case FNeg:
    flush_denormal = false;
    fn = [](const expr &v, FpRoundingMode rm){ return v.fneg(); };
    break;
  case Ceil:
    fn = [](const expr &v, FpRoundingMode rm) { return v.ceil(); };
    break;
  case Floor:
    fn = [](const expr &v, FpRoundingMode rm) { return v.floor(); };
    break;
  case RInt:
  case NearbyInt:
    // TODO: they differ in exception behavior
    fn = [](const expr &v, FpRoundingMode rm) { return v.round(rm.toSMT()); };
    break;
  case Round:
    fn = [](const expr &v, FpRoundingMode rm) { return v.round(expr::rna()); };
    break;
  case RoundEven:
    fn = [](const expr &v, FpRoundingMode rm) { return v.round(expr::rne()); };
    break;
  case Trunc:
    fn = [](const expr &v, FpRoundingMode rm) { return v.round(expr::rtz()); };
    break;
  case Sqrt:
    fn = [](const expr &v, FpRoundingMode rm) { return v.sqrt(rm.toSMT()); };
    break;
  }

  auto scalar = [&](const StateValue &v, const Type &ty) {
    return
      round_value([&](auto rm) {
        return fm_poison(s, v.value, v.non_poison,
                         [&](expr &v){ return fn(v, rm); }, ty, fmath,
                         !flush_denormal, flush_denormal);
      },  s, ty, rm, flush_denormal);
  };

  auto &v = s[*val];

  if (getType().isVectorType()) {
    vector<StateValue> vals;
    auto ty = val->getType().getAsAggregateType();
    for (unsigned i = 0, e = ty->numElementsConst(); i != e; ++i) {
      vals.emplace_back(scalar(ty->extract(v, i), ty->getChild(i)));
    }
    return getType().getAsAggregateType()->aggregateVals(vals);
  }
  return scalar(v, getType());
}

expr FpUnaryOp::getTypeConstraints(const Function &f) const {
  return Value::getTypeConstraints() &&
         getType() == val->getType() &&
         getType().enforceFloatOrVectorType();
}

unique_ptr<Instr> FpUnaryOp::dup(Function &f, const string &suffix) const {
  return
    make_unique<FpUnaryOp>(getType(), getName() + suffix, *val, op, fmath, rm);
}


vector<Value*> UnaryReductionOp::operands() const {
  return { val };
}

bool UnaryReductionOp::propagatesPoison() const {
  return true;
}

void UnaryReductionOp::rauw(const Value &what, Value &with) {
  RAUW(val);
}

void UnaryReductionOp::print(ostream &os) const {
  const char *str = nullptr;
  switch (op) {
  case Add:  str = "reduce_add "; break;
  case Mul:  str = "reduce_mul "; break;
  case And:  str = "reduce_and "; break;
  case Or:   str = "reduce_or "; break;
  case Xor:  str = "reduce_xor "; break;
  case SMax:  str = "reduce_smax "; break;
  case SMin:  str = "reduce_smin "; break;
  case UMax:  str = "reduce_umax "; break;
  case UMin:  str = "reduce_umin "; break;
  }

  os << getName() << " = " << str << print_type(val->getType())
     << val->getName();
}

StateValue UnaryReductionOp::toSMT(State &s) const {
  auto &v = s[*val];
  auto vty = val->getType().getAsAggregateType();
  StateValue res;
  for (unsigned i = 0, e = vty->numElementsConst(); i != e; ++i) {
    auto ith = vty->extract(v, i);
    if (i == 0) {
      res = std::move(ith);
      continue;
    }
    switch (op) {
    case Add: res.value = res.value + ith.value; break;
    case Mul: res.value = res.value * ith.value; break;
    case And: res.value = res.value & ith.value; break;
    case Or:  res.value = res.value | ith.value; break;
    case Xor: res.value = res.value ^ ith.value; break;
    case SMax: res.value = res.value.smax(ith.value); break;
    case SMin: res.value = res.value.smin(ith.value); break;
    case UMax: res.value = res.value.umax(ith.value); break;
    case UMin: res.value = res.value.umin(ith.value); break;
    }
    // The result is non-poisonous if all lanes are non-poisonous.
    res.non_poison &= ith.non_poison;
  }
  return res;
}

expr UnaryReductionOp::getTypeConstraints(const Function &f) const {
  return Value::getTypeConstraints() &&
         getType().enforceIntType() &&
         val->getType().enforceVectorType(
           [this](auto &scalar) { return scalar == getType(); });
}

unique_ptr<Instr>
UnaryReductionOp::dup(Function &f, const string &suffix) const {
  return make_unique<UnaryReductionOp>(getType(), getName() + suffix, *val, op);
}


vector<Value*> TernaryOp::operands() const {
  return { a, b, c };
}

bool TernaryOp::propagatesPoison() const {
  return true;
}

void TernaryOp::rauw(const Value &what, Value &with) {
  RAUW(a);
  RAUW(b);
  RAUW(c);
}

void TernaryOp::print(ostream &os) const {
  const char *str = nullptr;
  switch (op) {
  case FShl: str = "fshl "; break;
  case FShr: str = "fshr "; break;
  case SMulFix: str = "smul_fix "; break;
  case UMulFix: str = "umul_fix "; break;
  case SMulFixSat: str = "smul_fix_sat "; break;
  case UMulFixSat: str = "umul_fix_sat "; break;
  }

  os << getName() << " = " << str << *a << ", " << *b << ", " << *c;
}

StateValue TernaryOp::toSMT(State &s) const {
  auto &av = s[*a];
  auto &bv = s[*b];
  auto &cv = s[*c];

  auto scalar = [&](const auto &a, const auto &b, const auto &c) -> StateValue {
  expr e, np;
  switch (op) {
  case FShl:
    e = expr::fshl(a.value, b.value, c.value);
    np = true;
    break;
  case FShr:
    e = expr::fshr(a.value, b.value, c.value);
    np = true;
    break;
  case SMulFix:
    e = expr::smul_fix(a.value, b.value, c.value);
    np = expr::smul_fix_no_soverflow(a.value, b.value, c.value);
    break;
  case UMulFix:
    e = expr::umul_fix(a.value, b.value, c.value);
    np = expr::umul_fix_no_uoverflow(a.value, b.value, c.value);
    break;
  case SMulFixSat:
    e = expr::smul_fix_sat(a.value, b.value, c.value);
    np = true;
    break;
  case UMulFixSat:
    e = expr::umul_fix_sat(a.value, b.value, c.value);
    np = true;
    break;
  default:
    UNREACHABLE();
  }
  return { std::move(e), np && a.non_poison && b.non_poison && c.non_poison };
  };

  if (getType().isVectorType()) {
    vector<StateValue> vals;
    auto ty = getType().getAsAggregateType();

    for (unsigned i = 0, e = ty->numElementsConst(); i != e; ++i) {
      vals.emplace_back(scalar(ty->extract(av, i), ty->extract(bv, i),
                               (op == FShl || op == FShr) ?
                               ty->extract(cv, i) : cv));
    }
    return ty->aggregateVals(vals);
  }
  return scalar(av, bv, cv);
}

expr TernaryOp::getTypeConstraints(const Function &f) const {
  expr instrconstr;
  switch (op) {
  case FShl:
  case FShr:
    instrconstr =
      getType() == a->getType() &&
      getType() == b->getType() &&
      getType() == c->getType() &&
      getType().enforceIntOrVectorType();
    break;
  case SMulFix:
  case UMulFix:
  case SMulFixSat:
  case UMulFixSat:
    // LangRef only says that the third argument has to be an integer,
    // but the IR verifier seems to reject anything other than i32, so
    // we'll keep things simple and go with that constraint here too
    instrconstr =
      getType() == a->getType() &&
      getType() == b->getType() &&
      c->getType().enforceIntType(32) &&
      getType().enforceIntOrVectorType();
    break;
  default:
    UNREACHABLE();
  }
  return Value::getTypeConstraints() && instrconstr;
}

unique_ptr<Instr> TernaryOp::dup(Function &f, const string &suffix) const {
  return make_unique<TernaryOp>(getType(), getName() + suffix, *a, *b, *c, op);
}


vector<Value*> FpTernaryOp::operands() const {
  return { a, b, c };
}

bool FpTernaryOp::propagatesPoison() const {
  return true;
}

void FpTernaryOp::rauw(const Value &what, Value &with) {
  RAUW(a);
  RAUW(b);
  RAUW(c);
}

void FpTernaryOp::print(ostream &os) const {
  const char *str = nullptr;
  switch (op) {
  case FMA:    str = "fma "; break;
  case MulAdd: str = "fmuladd "; break;
  }

  os << getName() << " = " << str << fmath << *a << ", " << *b << ", " << *c;
  if (!rm.isDefault())
    os << ", rounding=" << rm;
  os << ", exceptions=" << ex;
}

StateValue FpTernaryOp::toSMT(State &s) const {
  function<expr(const expr&, const expr&, const expr&, FpRoundingMode)> fn;

  switch (op) {
  case FMA:
    fn = [](const expr &a, const expr &b, const expr &c, FpRoundingMode rm) {
      return expr::fma(a, b, c, rm.toSMT());
    };
    break;
  case MulAdd:
    fn = [&](const expr &a, const expr &b, const expr &c, FpRoundingMode rm0) {
      auto rm = rm0.toSMT();
      expr var = expr::mkFreshVar("nondet", expr(false));
      s.addQuantVar(var);
      return expr::mkIf(var, expr::fma(a, b, c, rm), a.fmul(b, rm).fadd(c, rm));
    };
    break;
  }

  auto scalar = [&](const StateValue &a, const StateValue &b,
                    const StateValue &c, const Type &ty) {
    return round_value([&](auto rm) {
      return fm_poison(s, a.value, a.non_poison, b.value, b.non_poison,
                       c.value, c.non_poison,
                       [&](expr &a, expr &b, expr &c){ return fn(a, b, c, rm);},
                       ty, fmath);
    }, s, ty, rm);
  };

  auto &av = s[*a];
  auto &bv = s[*b];
  auto &cv = s[*c];

  if (getType().isVectorType()) {
    vector<StateValue> vals;
    auto ty = getType().getAsAggregateType();

    for (unsigned i = 0, e = ty->numElementsConst(); i != e; ++i) {
      vals.emplace_back(scalar(ty->extract(av, i), ty->extract(bv, i),
                               ty->extract(cv, i), ty->getChild(i)));
    }
    return ty->aggregateVals(vals);
  }
  return scalar(av, bv, cv, getType());
}

expr FpTernaryOp::getTypeConstraints(const Function &f) const {
  return Value::getTypeConstraints() &&
         getType() == a->getType() &&
         getType() == b->getType() &&
         getType() == c->getType() &&
         getType().enforceFloatOrVectorType();
}

unique_ptr<Instr> FpTernaryOp::dup(Function &f, const string &suffix) const {
  return make_unique<FpTernaryOp>(getType(), getName() + suffix, *a, *b, *c, op,
                                  fmath, rm);
}


vector<Value*> TestOp::operands() const {
  return { lhs, rhs };
}

bool TestOp::propagatesPoison() const {
  return true;
}

void TestOp::rauw(const Value &what, Value &with) {
  RAUW(lhs);
  RAUW(rhs);
}

void TestOp::print(ostream &os) const {
  const char *str = nullptr;
  switch (op) {
  case Is_FPClass: str = "is.fpclass "; break;
  }

  os << getName() << " = " << str << *lhs << ", " << *rhs;
}

StateValue TestOp::toSMT(State &s) const {
  auto &a = s[*lhs];
  auto &b = s[*rhs];
  function<expr(const expr&)> fn;

  switch (op) {
  case Is_FPClass:
    fn = [&](const expr &a) -> expr {
      uint64_t n;
      if (!b.value.isUInt(n) || !b.non_poison.isTrue()) {
        s.addUB(expr(false));
        return {};
      }
      OrExpr result;
      // TODO: distinguish between quiet and signaling NaNs
      if (n & (1 << 0))
        result.add(a.isNaN());
      if (n & (1 << 1))
        result.add(a.isNaN());
      if (n & (1 << 2))
        result.add(a.isFPNegative() && a.isInf());
      if (n & (1 << 3))
        result.add(a.isFPNegative() && a.isFPNormal());
      if (n & (1 << 4))
        result.add(a.isFPNegative() && a.isFPSubNormal());
      if (n & (1 << 5))
        result.add(a.isFPNegZero());
      if (n & (1 << 6))
        result.add(a.isFPZero() && !a.isFPNegative());
      if (n & (1 << 7))
        result.add(!a.isFPNegative() && a.isFPSubNormal());
      if (n & (1 << 8))
        result.add(!a.isFPNegative() && a.isFPNormal());
      if (n & (1 << 9))
        result.add(!a.isFPNegative() && a.isInf());
      return result().toBVBool();
    };
    break;
  }

  auto scalar = [&](const StateValue &v) -> StateValue {
    return { fn(v.value), expr(v.non_poison) };
  };

  if (getType().isVectorType()) {
    vector<StateValue> vals;
    auto ty = lhs->getType().getAsAggregateType();

    for (unsigned i = 0, e = ty->numElementsConst(); i != e; ++i) {
      vals.emplace_back(scalar(ty->extract(a, i)));
    }
    return getType().getAsAggregateType()->aggregateVals(vals);
  }
  return scalar(a);
}

expr TestOp::getTypeConstraints(const Function &f) const {
  return Value::getTypeConstraints() &&
         lhs->getType().enforceFloatOrVectorType() &&
         rhs->getType().enforceIntType(32) &&
         getType().enforceIntOrVectorType(1) &&
         getType().enforceVectorTypeEquiv(lhs->getType());
}

unique_ptr<Instr> TestOp::dup(Function &f, const string &suffix) const {
  return make_unique<TestOp>(getType(), getName() + suffix, *lhs, *rhs, op);
}


vector<Value*> ConversionOp::operands() const {
  return { val };
}

bool ConversionOp::propagatesPoison() const {
  return true;
}

void ConversionOp::rauw(const Value &what, Value &with) {
  RAUW(val);
}

void ConversionOp::print(ostream &os) const {
  const char *str = nullptr;
  switch (op) {
  case SExt:     str = "sext "; break;
  case ZExt:     str = "zext "; break;
  case Trunc:    str = "trunc "; break;
  case BitCast:  str = "bitcast "; break;
  case Ptr2Int:  str = "ptrtoint "; break;
  case Int2Ptr:  str = "int2ptr "; break;
  }

  os << getName() << " = " << str << *val << print_type(getType(), " to ", "");
}

StateValue ConversionOp::toSMT(State &s) const {
  auto v = s[*val];
  function<expr(expr &&, const Type &)> fn;

  switch (op) {
  case SExt:
    fn = [](auto &&val, auto &to_type) -> expr {
      return val.sext(to_type.bits() - val.bits());
    };
    break;
  case ZExt:
    fn = [](auto &&val, auto &to_type) -> expr {
      return val.zext(to_type.bits() - val.bits());
    };
    break;
  case Trunc:
    fn = [](auto &&val, auto &to_type) -> expr {
      return val.trunc(to_type.bits());
    };
    break;
  case BitCast:
    fn = [](auto &&val, auto &to_type) -> expr {
      return to_type.fromInt(std::move(val));
    };
    break;
  case Ptr2Int:
    fn = [&](auto &&val, auto &to_type) -> expr {
      return s.getMemory().ptr2int(val).zextOrTrunc(to_type.bits());
    };
    break;
  case Int2Ptr:
    fn = [&](auto &&val, auto &to_type) -> expr {
      return s.getMemory().int2ptr(val);
    };
    break;
  }

  if (op == BitCast) {
    // NOP: ptr vect -> ptr vect
    if (getType().isVectorType() &&
        getType().getAsAggregateType()->getChild(0).isPtrType())
      return v;

    v = val->getType().toInt(s, std::move(v));
  }

  if (getType().isVectorType()) {
    vector<StateValue> vals;
    auto retty = getType().getAsAggregateType();
    auto elems = retty->numElementsConst();

    // bitcast vect elems size may vary, so create a new data type whose
    // element size is aligned with the output vector elem size
    IntType elem_ty("int", retty->bits() / elems);
    VectorType int_ty("vec", elems, elem_ty);
    auto valty = op == BitCast ? &int_ty : val->getType().getAsAggregateType();

    for (unsigned i = 0; i != elems; ++i) {
      unsigned idx = (little_endian && op == BitCast) ? elems - i - 1 : i;
      auto vi = valty->extract(v, idx);
      vals.emplace_back(fn(std::move(vi.value), retty->getChild(idx)),
                        std::move(vi.non_poison));
    }
    return retty->aggregateVals(vals);
  }

  // turn poison data into boolean
  if (op == BitCast)
    v.non_poison = v.non_poison == expr::mkInt(-1, v.non_poison);

  return { fn(std::move(v.value), getType()), std::move(v.non_poison) };
}

expr ConversionOp::getTypeConstraints(const Function &f) const {
  expr c;
  switch (op) {
  case SExt:
  case ZExt:
    c = getType().enforceIntOrVectorType() &&
        val->getType().enforceIntOrVectorType() &&
        val->getType().scalarSize().ult(getType().scalarSize());
    break;
  case Trunc:
    c = getType().enforceIntOrVectorType() &&
        val->getType().enforceIntOrVectorType() &&
        getType().scalarSize().ult(val->getType().scalarSize());
    break;
  case BitCast:
    c = getType().enforceIntOrFloatOrPtrOrVectorType() &&
        val->getType().enforceIntOrFloatOrPtrOrVectorType() &&
        getType().enforcePtrOrVectorType() ==
          val->getType().enforcePtrOrVectorType() &&
        getType().sizeVar() == val->getType().sizeVar();
    break;
  case Ptr2Int:
    c = getType().enforceIntOrVectorType() &&
        val->getType().enforcePtrOrVectorType();
    break;
  case Int2Ptr:
    c = getType().enforcePtrOrVectorType() &&
        val->getType().enforceIntOrVectorType();
    break;
  }

  c &= Value::getTypeConstraints();
  if (op != BitCast)
    c &= getType().enforceVectorTypeEquiv(val->getType());
  return c;
}

unique_ptr<Instr> ConversionOp::dup(Function &f, const string &suffix) const {
  return make_unique<ConversionOp>(getType(), getName() + suffix, *val, op);
}


vector<Value*> FpConversionOp::operands() const {
  return { val };
}

bool FpConversionOp::propagatesPoison() const {
  return true;
}

void FpConversionOp::rauw(const Value &what, Value &with) {
  RAUW(val);
}

void FpConversionOp::print(ostream &os) const {
  const char *str = nullptr;
  switch (op) {
  case SIntToFP: str = "sitofp "; break;
  case UIntToFP: str = "uitofp "; break;
  case FPToSInt: str = "fptosi "; break;
  case FPToUInt: str = "fptoui "; break;
  case FPExt:    str = "fpext "; break;
  case FPTrunc:  str = "fptrunc "; break;
  case LRInt:    str = "lrint "; break;
  case LRound:   str = "lround "; break;
  }

  os << getName() << " = " << str << *val << print_type(getType(), " to ", "");
  if (!rm.isDefault())
    os << ", rounding=" << rm;
  os << ", exceptions=" << ex;
}

StateValue FpConversionOp::toSMT(State &s) const {
  auto &v = s[*val];
  function<StateValue(const expr &, const Type &, FpRoundingMode)> fn;

  switch (op) {
  case SIntToFP:
    fn = [](auto &val, auto &to_type, auto rm) -> StateValue {
      return
        { val.sint2fp(to_type.getDummyValue(false).value, rm.toSMT()), true };
    };
    break;
  case UIntToFP:
    fn = [](auto &val, auto &to_type, auto rm) -> StateValue {
      return
        { val.uint2fp(to_type.getDummyValue(false).value, rm.toSMT()), true };
    };
    break;
  case FPToSInt:
  case LRInt:
  case LRound:
    fn = [&](auto &val, auto &to_type, auto rm_in) -> StateValue {
      expr rm;
      switch (op) {
      case FPToSInt:
        rm = expr::rtz();
        break;
      case LRInt:
        rm = rm_in.toSMT();
        break;
      case LRound:
        rm = expr::rna();
        break;
      default: UNREACHABLE();
      }
      expr bv  = val.fp2sint(to_type.bits(), rm);
      expr fp2 = bv.sint2fp(val, rm);
      // -0.xx is converted to 0 and then to 0.0, though -0.xx is ok to convert
      expr val_rounded = val.round(rm);
      return { std::move(bv), val_rounded.isFPZero() || fp2 == val_rounded };
    };
    break;
  case FPToUInt:
    fn = [](auto &val, auto &to_type, auto rm_) -> StateValue {
      expr rm = expr::rtz();
      expr bv  = val.fp2uint(to_type.bits(), rm);
      expr fp2 = bv.uint2fp(val, rm);
      // -0.xx must be converted to 0, not poison.
      expr val_rounded = val.round(rm);
      return { std::move(bv), val_rounded.isFPZero() || fp2 == val_rounded };
    };
    break;
  case FPExt:
  case FPTrunc:
    fn = [](auto &val, auto &to_type, auto rm) -> StateValue {
      return { val.float2Float(to_type.getDummyValue(false).value, rm.toSMT()),
               true };
    };
    break;
  }

  auto scalar = [&](const StateValue &sv, const Type &to_type) -> StateValue {
    auto [v, np]
      = round_value([&](auto rm) { return fn(sv.value, to_type, rm); }, s,
                    to_type, rm);
    return { std::move(v), sv.non_poison && np };
  };

  if (getType().isVectorType()) {
    vector<StateValue> vals;
    auto ty = val->getType().getAsAggregateType();
    auto retty = getType().getAsAggregateType();

    for (unsigned i = 0, e = ty->numElementsConst(); i != e; ++i) {
      vals.emplace_back(scalar(ty->extract(v, i), retty->getChild(i)));
    }
    return retty->aggregateVals(vals);
  }
  return scalar(v, getType());
}

expr FpConversionOp::getTypeConstraints(const Function &f) const {
  expr c;
  switch (op) {
  case SIntToFP:
  case UIntToFP:
    c = getType().enforceFloatOrVectorType() &&
        val->getType().enforceIntOrVectorType();
    break;
  case FPToSInt:
  case FPToUInt:
  case LRInt:
  case LRound:
    c = getType().enforceIntOrVectorType() &&
        val->getType().enforceFloatOrVectorType();
    break;
  case FPExt:
    c = getType().enforceFloatOrVectorType() &&
        val->getType().enforceFloatOrVectorType() &&
        val->getType().scalarSize().ult(getType().scalarSize());
    break;
  case FPTrunc:
    c = getType().enforceFloatOrVectorType() &&
        val->getType().enforceFloatOrVectorType() &&
        val->getType().scalarSize().ugt(getType().scalarSize());
    break;
  }
  return Value::getTypeConstraints() && c;
}

unique_ptr<Instr> FpConversionOp::dup(Function &f, const string &suffix) const {
  return
    make_unique<FpConversionOp>(getType(), getName() + suffix, *val, op, rm);
}


vector<Value*> Select::operands() const {
  return { cond, a, b };
}

void Select::rauw(const Value &what, Value &with) {
  RAUW(cond);
  RAUW(a);
  RAUW(b);
}

void Select::print(ostream &os) const {
  os << getName() << " = select " << fmath << *cond << ", " << *a << ", " << *b;
}

StateValue Select::toSMT(State &s) const {
  auto &cv = s[*cond];
  auto &av = s[*a];
  auto &bv = s[*b];

  auto scalar = [&](const auto &a, const auto &b, const auto &c) -> StateValue {
    auto cond = c.value == 1;
    auto identity = [](const expr &x) { return x; };
    StateValue sva = fm_poison(s, a.value, a.non_poison, identity, getType(),
                               fmath, true, false);
    StateValue svb = fm_poison(s, b.value, b.non_poison, identity, getType(),
                               fmath, true, false);
    return { expr::mkIf(cond, sva.value, svb.value),
             c.non_poison && expr::mkIf(cond, sva.non_poison, svb.non_poison) };
  };

  if (auto agg = getType().getAsAggregateType()) {
    vector<StateValue> vals;
    auto cond_agg = cond->getType().getAsAggregateType();

    for (unsigned i = 0, e = agg->numElementsConst(); i != e; ++i) {
      if (!agg->isPadding(i))
        vals.emplace_back(scalar(agg->extract(av, i), agg->extract(bv, i),
                                 cond_agg ? cond_agg->extract(cv, i) : cv));
    }
    return agg->aggregateVals(vals);
  }
  return scalar(av, bv, cv);
}

expr Select::getTypeConstraints(const Function &f) const {
  return Value::getTypeConstraints() &&
         cond->getType().enforceIntOrVectorType(1) &&
         getType().enforceVectorTypeIff(cond->getType()) &&
         (fmath.isNone() ? expr(true) : getType().enforceFloatOrVectorType()) &&
         getType() == a->getType() &&
         getType() == b->getType();
}

unique_ptr<Instr> Select::dup(Function &f, const string &suffix) const {
  return make_unique<Select>(getType(), getName() + suffix, *cond, *a, *b);
}


void ExtractValue::addIdx(unsigned idx) {
  idxs.emplace_back(idx);
}

vector<Value*> ExtractValue::operands() const {
  return { val };
}

void ExtractValue::rauw(const Value &what, Value &with) {
  RAUW(val);
}

void ExtractValue::print(ostream &os) const {
  os << getName() << " = extractvalue " << *val;
  for (auto idx : idxs) {
    os << ", " << idx;
  }
}

StateValue ExtractValue::toSMT(State &s) const {
  auto v = s[*val];

  Type *type = &val->getType();
  for (auto idx : idxs) {
    auto ty = type->getAsAggregateType();
    v = ty->extract(v, idx);
    type = &ty->getChild(idx);
  }
  return v;
}

expr ExtractValue::getTypeConstraints(const Function &f) const {
  auto c = Value::getTypeConstraints() &&
           val->getType().enforceAggregateType();

  Type *type = &val->getType();
  unsigned i = 0;
  for (auto idx : idxs) {
    auto ty = type->getAsAggregateType();
    if (!ty) {
      c = false;
      break;
    }
    type = &ty->getChild(idx);

    c &= ty->numElements().ugt(idx);
    if (++i == idxs.size() && idx < ty->numElementsConst())
      c &= ty->getChild(idx) == getType();
  }
  return c;
}

unique_ptr<Instr> ExtractValue::dup(Function &f, const string &suffix) const {
  auto ret = make_unique<ExtractValue>(getType(), getName() + suffix, *val);
  for (auto idx : idxs) {
    ret->addIdx(idx);
  }
  return ret;
}


void InsertValue::addIdx(unsigned idx) {
  idxs.emplace_back(idx);
}

vector<Value*> InsertValue::operands() const {
  return { val, elt };
}

void InsertValue::rauw(const Value &what, Value &with) {
  RAUW(val);
  RAUW(elt);
}

void InsertValue::print(ostream &os) const {
  os << getName() << " = insertvalue " << *val << ", " << *elt;
  for (auto idx : idxs) {
    os << ", " << idx;
  }
}

static StateValue update_repack(Type *type,
                                const StateValue &val,
                                const StateValue &elem,
                                vector<unsigned> &indices) {
  auto ty = type->getAsAggregateType();
  unsigned cur_idx = indices.back();
  indices.pop_back();
  vector<StateValue> vals;
  for (unsigned i = 0, e = ty->numElementsConst(); i < e; ++i) {
    if (ty->isPadding(i))
      continue;

    auto v = ty->extract(val, i);
    if (i == cur_idx) {
      vals.emplace_back(indices.empty() ?
                        elem :
                        update_repack(&ty->getChild(i), v, elem, indices));
    } else {
      vals.emplace_back(std::move(v));
    }
  }

  return ty->aggregateVals(vals);
}

StateValue InsertValue::toSMT(State &s) const {
  auto &sv = s[*val];
  auto &elem = s[*elt];

  Type *type = &val->getType();
  vector<unsigned> idxs_reverse(idxs.rbegin(), idxs.rend());
  return update_repack(type, sv, elem, idxs_reverse);
}

expr InsertValue::getTypeConstraints(const Function &f) const {
  auto c = Value::getTypeConstraints() &&
           val->getType().enforceAggregateType() &&
           val->getType() == getType();

  Type *type = &val->getType();
  unsigned i = 0;
  for (auto idx : idxs) {
    auto ty = type->getAsAggregateType();
    if (!ty)
      return false;

    type = &ty->getChild(idx);

    c &= ty->numElements().ugt(idx);
    if (++i == idxs.size() && idx < ty->numElementsConst())
      c &= ty->getChild(idx) == elt->getType();
  }

  return c;
}

unique_ptr<Instr> InsertValue::dup(Function &f, const string &suffix) const {
  auto ret = make_unique<InsertValue>(getType(), getName() + suffix, *val, *elt);
  for (auto idx : idxs) {
    ret->addIdx(idx);
  }
  return ret;
}

DEFINE_AS_RETZERO(FnCall, getMaxGEPOffset);

pair<uint64_t, uint64_t> FnCall::getMaxAllocSize() const {
  if (!hasAttribute(FnAttrs::AllocSize))
    return { 0, 1 };

  if (auto sz = getInt(*args[attrs.allocsize_0].first)) {
    if (attrs.allocsize_1 == -1u)
      return { *sz, getAlign() };

    if (auto n = getInt(*args[attrs.allocsize_1].first))
      return { mul_saturate(*sz, *n), getAlign() };
  }
  return { UINT64_MAX, getAlign() };
}

static Value* get_align_arg(const vector<pair<Value*, ParamAttrs>> args) {
  for (auto &[arg, attrs] : args) {
    if (attrs.has(ParamAttrs::AllocAlign))
      return arg;
  }
  return nullptr;
}

Value* FnCall::getAlignArg() const {
  return get_align_arg(args);
}

uint64_t FnCall::getAlign() const {
  uint64_t align = 0;
  // TODO: add support for non constant alignments
  if (auto *arg = getAlignArg())
    align = getIntOr(*arg, 0);

  return max(align, attrs.align ? attrs.align : heap_block_alignment);
}

uint64_t FnCall::getMaxAccessSize() const {
  uint64_t sz = attrs.has(FnAttrs::Dereferenceable) ? attrs.derefBytes : 0;
  if (attrs.has(FnAttrs::DereferenceableOrNull))
    sz = max(sz, attrs.derefOrNullBytes);

  for (auto &[arg, attrs] : args) {
    if (attrs.has(ParamAttrs::Dereferenceable))
      sz = max(sz, attrs.derefBytes);
    if (attrs.has(ParamAttrs::DereferenceableOrNull))
      sz = max(sz, attrs.derefOrNullBytes);
  }
  return sz;
}

MemInstr::ByteAccessInfo FnCall::getByteAccessInfo() const {
  if (attrs.has(AllocKind::Uninitialized) || attrs.has(AllocKind::Free))
    return {};

  // calloc style
  if (attrs.has(AllocKind::Zeroed)) {
    auto info = ByteAccessInfo::intOnly(1);
    auto [alloc, align] = getMaxAllocSize();
    if (alloc)
      info.byteSize = gcd(alloc, align);
    return info;
  }

  // If bytesize is zero, this call does not participate in byte encoding.
  uint64_t bytesize = 0;

#define UPDATE(attr)                                                   \
  do {                                                                 \
    uint64_t sz = 0;                                                   \
    if (attr.has(decay<decltype(attr)>::type::Dereferenceable))        \
      sz = attr.derefBytes;                                            \
    if (attr.has(decay<decltype(attr)>::type::DereferenceableOrNull))  \
      sz = gcd(sz, attr.derefOrNullBytes);                             \
    if (sz) {                                                          \
      sz = gcd(sz, retattr.align ? retattr.align : 1);                 \
      bytesize = bytesize ? gcd(bytesize, sz) : sz;                    \
    }                                                                  \
  } while (0)

  auto &retattr = getAttributes();
  UPDATE(retattr);

  for (auto &[arg, attrs] : args) {
    if (!arg->getType().isPtrType())
      continue;

    UPDATE(attrs);
    // Pointer arguments without dereferenceable attr don't contribute to the
    // byte size.
    // call f(* dereferenceable(n) align m %p, * %q) is equivalent to a dummy
    // load followed by a function call:
    //   load i<8*n> %p, align m
    //   call f(* %p, * %q)
    // f(%p, %q) does not contribute to the bytesize. After bytesize is fixed,
    // function calls update a memory with the granularity.
  }
#undef UPDATE

  // No dereferenceable attribute
  if (bytesize == 0)
    return {};

  return ByteAccessInfo::anyType(bytesize);
}


void FnCall::addArg(Value &arg, ParamAttrs &&attrs) {
  args.emplace_back(&arg, std::move(attrs));
}

vector<Value*> FnCall::operands() const {
  vector<Value*> output;
  transform(args.begin(), args.end(), back_inserter(output),
            [](auto &p){ return p.first; });
  return output;
}

void FnCall::rauw(const Value &what, Value &with) {
  for (auto &arg : args) {
    RAUW(arg.first);
  }
}

void FnCall::print(ostream &os) const {
  if (!isVoid())
    os << getName() << " = ";

  os << "call " << print_type(getType()) << fnName << '(';

  bool first = true;
  for (auto &[arg, attrs] : args) {
    if (!first)
      os << ", ";

    os << attrs << *arg;
    first = false;
  }
  os << ')' << attrs;
}

static void eq_bids(OrExpr &acc, Memory &m, const Type &t,
                    const StateValue &val, const expr &bid) {
  if (auto agg = t.getAsAggregateType()) {
    for (unsigned i = 0, e = agg->numElementsConst(); i != e; ++i) {
      eq_bids(acc, m, agg->getChild(i), agg->extract(val, i), bid);
    }
    return;
  }

  if (t.isPtrType()) {
    acc.add(val.non_poison && Pointer(m, val.value).getBid() == bid);
  }
}

static expr ptr_only_args(State &s, const Pointer &p) {
  expr bid = p.getBid();
  auto &m  = s.getMemory();

  OrExpr e;
  for (auto &in : s.getFn().getInputs()) {
    if (hasPtr(in.getType()))
      eq_bids(e, m, in.getType(), s[in], bid);
  }
  return e();
}

static void check_can_load(State &s, const expr &p0) {
  auto &attrs = s.getFn().getFnAttrs();
  Pointer p(s.getMemory(), p0);

  if (attrs.has(FnAttrs::NoRead))
    s.addUB(p.isLocal() || p.isConstGlobal());
  else if (attrs.has(FnAttrs::ArgMemOnly))
    s.addUB(p.isLocal() || ptr_only_args(s, p));
}

static void check_can_store(State &s, const expr &p0) {
  if (s.isInitializationPhase())
    return;

  auto &attrs = s.getFn().getFnAttrs();
  Pointer p(s.getMemory(), p0);

  if (attrs.has(FnAttrs::NoWrite))
    s.addUB(p.isLocal());
  else if (attrs.has(FnAttrs::ArgMemOnly))
    s.addUB(p.isLocal() || ptr_only_args(s, p));
}

static void unpack_inputs(State &s, Value &argv, Type &ty,
                          const ParamAttrs &argflag, bool argmemonly,
                          StateValue value, StateValue value2,
                          vector<StateValue> &inputs,
                          vector<Memory::PtrInput> &ptr_inputs) {
  if (auto agg = ty.getAsAggregateType()) {
    for (unsigned i = 0, e = agg->numElementsConst(); i != e; ++i) {
      unpack_inputs(s, argv, agg->getChild(i), argflag, argmemonly,
                    agg->extract(value, i), agg->extract(value2, i), inputs,
                    ptr_inputs);
    }
    return;
  }

  auto unpack = [&](StateValue &&value) {
    value = argflag.encode(s, std::move(value), ty);

    if (ty.isPtrType()) {
      if (argmemonly)
        value.non_poison
          &= ptr_only_args(s, Pointer(s.getMemory(), value.value));

      ptr_inputs.emplace_back(std::move(value),
                              argflag.blockSize,
                              argflag.has(ParamAttrs::NoRead),
                              argflag.has(ParamAttrs::NoWrite),
                              argflag.has(ParamAttrs::NoCapture));
    } else {
      inputs.emplace_back(std::move(value));
    }
  };
  unpack(std::move(value));
  unpack(std::move(value2));
}

static void unpack_ret_ty (vector<Type*> &out_types, Type &ty) {
  if (auto agg = ty.getAsAggregateType()) {
    for (unsigned i = 0, e = agg->numElementsConst(); i != e; ++i) {
      // Padding is automatically filled with poison
      if (agg->isPadding(i))
        continue;
      unpack_ret_ty(out_types, agg->getChild(i));
    }
  } else {
    out_types.emplace_back(&ty);
  }
}

static StateValue
check_return_value(State &s, StateValue &&val, const Type &ty,
                   const FnAttrs &attrs,
                   const vector<pair<Value*, ParamAttrs>> &args) {
  auto [allocsize, np] = attrs.computeAllocSize(s, args);
  s.addUB(std::move(np));
  return attrs.encode(s, std::move(val), ty, allocsize, get_align_arg(args));
}

static StateValue
pack_return(State &s, Type &ty, vector<StateValue> &vals, const FnAttrs &attrs,
            unsigned &idx, const vector<pair<Value*, ParamAttrs>> &args) {
  if (auto agg = ty.getAsAggregateType()) {
    vector<StateValue> vs;
    for (unsigned i = 0, e = agg->numElementsConst(); i != e; ++i) {
      if (!agg->isPadding(i))
        vs.emplace_back(
          pack_return(s, agg->getChild(i), vals, attrs, idx, args));
    }
    return agg->aggregateVals(vs);
  }

  return check_return_value(s, std::move(vals[idx++]), ty, attrs, args);
}

StateValue FnCall::toSMT(State &s) const {
  if (approx)
    s.doesApproximation("Unknown libcall: " + fnName);

  auto &m = s.getMemory();

  vector<StateValue> inputs;
  vector<Memory::PtrInput> ptr_inputs;
  vector<Type*> out_types;
  bool argmemonly_fn   = s.getFn().getFnAttrs().has(FnAttrs::ArgMemOnly);
  bool argmemonly_call = hasAttribute(FnAttrs::ArgMemOnly);

  ostringstream fnName_mangled;
  fnName_mangled << fnName;
  for (auto &[arg, flags] : args) {
    // we duplicate each argument so that undef values are allowed to take
    // different values so we can catch the bug in f(freeze(undef)) -> f(undef)
    StateValue sv, sv2;
    if (flags.poisonImpliesUB()) {
      sv = s.getAndAddPoisonUB(*arg, flags.undefImpliesUB());
      if (flags.undefImpliesUB())
        sv2 = sv;
      else
        sv2 = s.getAndAddPoisonUB(*arg, false);
    } else {
      sv  = s[*arg];
      sv2 = s[*arg];
    }

    unpack_inputs(s, *arg, arg->getType(), flags, argmemonly_fn, std::move(sv),
                  std::move(sv2), inputs, ptr_inputs);
    fnName_mangled << '#' << arg->getType();
  }
  fnName_mangled << '!' << getType();
  if (!isVoid())
    unpack_ret_ty(out_types, getType());

  auto check = [&](FnAttrs::Attribute attr) {
    return s.getFn().getFnAttrs().has(attr) && !hasAttribute(attr);
  };

  auto check_implies = [&](FnAttrs::Attribute attr) {
    if (!check(attr))
      return;

    if (argmemonly_call) {
      for (auto &p : ptr_inputs) {
        if (!p.byval) {
          Pointer ptr(m, p.val.value);
          s.addUB(p.val.non_poison.implies(
                    ptr.isLocal() || ptr.isConstGlobal()));
        }
      }
    } else {
      s.addUB(expr(false));
    }
  };

  check_implies(FnAttrs::NoRead);
  check_implies(FnAttrs::NoWrite);

  // Check attributes that calles must have if caller has them
  if (check(FnAttrs::ArgMemOnly) ||
      check(FnAttrs::NoThrow) ||
      check(FnAttrs::WillReturn) ||
      check(FnAttrs::InaccessibleMemOnly))
    s.addUB(expr(false));

  // can't have both!
  if (attrs.has(FnAttrs::ArgMemOnly) && attrs.has(FnAttrs::InaccessibleMemOnly))
    s.addUB(expr(false));

  auto get_alloc_ptr = [&]() -> Value& {
    for (auto &[arg, flags] : args) {
      if (flags.has(ParamAttrs::AllocPtr))
        return *arg;
    }
    UNREACHABLE();
  };

  if (attrs.has(AllocKind::Alloc) || attrs.has(AllocKind::Realloc)) {
    auto [size, np_size] = attrs.computeAllocSize(s, args);
    expr nonnull = attrs.isNonNull() ? expr(true)
                                     : expr::mkBoolVar("malloc_never_fails");
    // FIXME: alloc-family below
    auto [p_new, allocated]
      = m.alloc(size, getAlign(), Memory::MALLOC, np_size, nonnull);

    expr nullp = Pointer::mkNullPointer(m)();
    expr ret = expr::mkIf(allocated, p_new, nullp);

    // TODO: In C++ we need to throw an exception if the allocation fails.

    if (attrs.has(AllocKind::Realloc)) {
      auto &[allocptr, np_ptr] = s.getAndAddUndefs(get_alloc_ptr());
      s.addUB(np_ptr);

      check_can_store(s, allocptr);

      Pointer ptr_old(m, allocptr);
      if (s.getFn().getFnAttrs().has(FnAttrs::NoFree))
        s.addUB(ptr_old.isNull() || ptr_old.isLocal());

      m.copy(ptr_old, Pointer(m, p_new));

      // 1) realloc(ptr, 0) always free the ptr.
      // 2) If allocation failed, we should not free previous ptr, unless it's
      // reallocf (always frees the pointer)
      expr freeptr = fnName == "@reallocf"
                       ? allocptr
                       : expr::mkIf(size == 0 || allocated, allocptr, nullp);
      m.free(freeptr, false);
    }

    // FIXME: for a realloc that zeroes the new stuff
    if (attrs.has(AllocKind::Zeroed))
      m.memset(p_new, { expr::mkUInt(0, 8), true }, size, getAlign(), {},
               false);

    assert(getType().isPtrType());
    return attrs.encode(s, {std::move(ret), true}, getType(), size,
                        getAlignArg());
  }
  else if (attrs.has(AllocKind::Free)) {
    auto &allocptr = s.getAndAddPoisonUB(get_alloc_ptr()).value;
    m.free(allocptr, false);

    if (s.getFn().getFnAttrs().has(FnAttrs::NoFree)) {
      Pointer ptr(m, allocptr);
      s.addUB(ptr.isNull() || ptr.isLocal());
    }
    assert(isVoid());
    return {};
  }

  check_implies(FnAttrs::NoFree);

  unsigned idx = 0;
  auto ret = s.addFnCall(fnName_mangled.str(), std::move(inputs),
                         std::move(ptr_inputs), out_types, attrs);

  return isVoid() ? StateValue()
                  : pack_return(s, getType(), ret, attrs, idx, args);
}

expr FnCall::getTypeConstraints(const Function &f) const {
  // TODO : also need to name each arg type smt var uniquely
  return Value::getTypeConstraints();
}

unique_ptr<Instr> FnCall::dup(Function &f, const string &suffix) const {
  auto r = make_unique<FnCall>(getType(), getName() + suffix, string(fnName),
                               FnAttrs(attrs));
  r->args = args;
  r->approx = approx;
  return r;
}


InlineAsm::InlineAsm(Type &type, string &&name, const string &asm_str,
                     const string &constraints, FnAttrs &&attrs)
  : FnCall(type, std::move(name), "asm " + asm_str + ", " + constraints,
           std::move(attrs)) {}


ICmp::ICmp(Type &type, string &&name, Cond cond, Value &a, Value &b)
  : Instr(type, std::move(name)), a(&a), b(&b), cond(cond), defined(cond != Any) {
  if (!defined)
    cond_name = getName() + "_cond";
}

expr ICmp::cond_var() const {
  return defined ? expr::mkUInt(cond, 4) : expr::mkVar(cond_name.c_str(), 4);
}

vector<Value*> ICmp::operands() const {
  return { a, b };
}

bool ICmp::propagatesPoison() const {
  return true;
}

bool ICmp::isPtrCmp() const {
  auto &elem_ty = a->getType();
  return elem_ty.isPtrType() ||
      (elem_ty.isVectorType() &&
       elem_ty.getAsAggregateType()->getChild(0).isPtrType());
}

void ICmp::rauw(const Value &what, Value &with) {
  RAUW(a);
  RAUW(b);
}

void ICmp::print(ostream &os) const {
  const char *condtxt = nullptr;
  switch (cond) {
  case EQ:  condtxt = "eq "; break;
  case NE:  condtxt = "ne "; break;
  case SLE: condtxt = "sle "; break;
  case SLT: condtxt = "slt "; break;
  case SGE: condtxt = "sge "; break;
  case SGT: condtxt = "sgt "; break;
  case ULE: condtxt = "ule "; break;
  case ULT: condtxt = "ult "; break;
  case UGE: condtxt = "uge "; break;
  case UGT: condtxt = "ugt "; break;
  case Any: condtxt = ""; break;
  }
  os << getName() << " = icmp " << condtxt << *a << ", " << b->getName();
  switch (pcmode) {
  case INTEGRAL: break;
  case PROVENANCE: os << ", use_provenance"; break;
  case OFFSETONLY: os << ", offsetonly"; break;
  }
}

static expr build_icmp_chain(const expr &var,
                             const function<expr(ICmp::Cond)> &fn,
                             ICmp::Cond cond = ICmp::Any,
                             expr last = expr()) {
  auto old_cond = cond;
  cond = ICmp::Cond(cond - 1);

  if (old_cond == ICmp::Any)
    return build_icmp_chain(var, fn, cond, fn(cond));

  auto e = expr::mkIf(var == cond, fn(cond), last);
  return cond == 0 ? e : build_icmp_chain(var, fn, cond, std::move(e));
}

StateValue ICmp::toSMT(State &s) const {
  auto &a_eval = s[*a];
  auto &b_eval = s[*b];

  function<expr(const expr&, const expr&, Cond)> fn =
      [&](auto &av, auto &bv, Cond cond) {
    switch (cond) {
    case EQ:  return av == bv;
    case NE:  return av != bv;
    case SLE: return av.sle(bv);
    case SLT: return av.slt(bv);
    case SGE: return av.sge(bv);
    case SGT: return av.sgt(bv);
    case ULE: return av.ule(bv);
    case ULT: return av.ult(bv);
    case UGE: return av.uge(bv);
    case UGT: return av.ugt(bv);
    case Any:
      UNREACHABLE();
    }
    UNREACHABLE();
  };

  if (isPtrCmp()) {
    fn = [this, &s, fn](const expr &av, const expr &bv, Cond cond) {
      Pointer lhs(s.getMemory(), av);
      Pointer rhs(s.getMemory(), bv);
      switch (pcmode) {
      case INTEGRAL:
        return fn(lhs.getAddress(), rhs.getAddress(), cond);
      case PROVENANCE:
        assert(cond == EQ || cond == NE);
        return cond == EQ ? lhs == rhs : lhs != rhs;
      case OFFSETONLY:
        return fn(lhs.getOffset(), rhs.getOffset(), cond);
      }
      UNREACHABLE();
    };
  }

  auto scalar = [&](const StateValue &a, const StateValue &b) -> StateValue {
    auto fn2 = [&](Cond c) { return fn(a.value, b.value, c); };
    auto v = cond != Any ? fn2(cond) : build_icmp_chain(cond_var(), fn2);
    return { v.toBVBool(), a.non_poison && b.non_poison };
  };

  auto &elem_ty = a->getType();
  if (auto agg = elem_ty.getAsAggregateType()) {
    vector<StateValue> vals;
    for (unsigned i = 0, e = agg->numElementsConst(); i != e; ++i) {
      vals.emplace_back(scalar(agg->extract(a_eval, i),
                               agg->extract(b_eval, i)));
    }
    return getType().getAsAggregateType()->aggregateVals(vals);
  }
  return scalar(a_eval, b_eval);
}

expr ICmp::getTypeConstraints(const Function &f) const {
  return Value::getTypeConstraints() &&
         getType().enforceIntOrVectorType(1) &&
         getType().enforceVectorTypeEquiv(a->getType()) &&
         a->getType().enforceIntOrPtrOrVectorType() &&
         a->getType() == b->getType();
}

unique_ptr<Instr> ICmp::dup(Function &f, const string &suffix) const {
  return make_unique<ICmp>(getType(), getName() + suffix, cond, *a, *b);
}


vector<Value*> FCmp::operands() const {
  return { a, b };
}

void FCmp::rauw(const Value &what, Value &with) {
  RAUW(a);
  RAUW(b);
}

void FCmp::print(ostream &os) const {
  const char *condtxt = nullptr;
  switch (cond) {
  case OEQ:   condtxt = "oeq "; break;
  case OGT:   condtxt = "ogt "; break;
  case OGE:   condtxt = "oge "; break;
  case OLT:   condtxt = "olt "; break;
  case OLE:   condtxt = "ole "; break;
  case ONE:   condtxt = "one "; break;
  case ORD:   condtxt = "ord "; break;
  case UEQ:   condtxt = "ueq "; break;
  case UGT:   condtxt = "ugt "; break;
  case UGE:   condtxt = "uge "; break;
  case ULT:   condtxt = "ult "; break;
  case ULE:   condtxt = "ule "; break;
  case UNE:   condtxt = "une "; break;
  case UNO:   condtxt = "uno "; break;
  case TRUE:  condtxt = "true "; break;
  case FALSE: condtxt = "false "; break;
  }
  os << getName() << " = fcmp " << fmath << condtxt << *a << ", "
     << b->getName();
}

StateValue FCmp::toSMT(State &s) const {
  auto &a_eval = s[*a];
  auto &b_eval = s[*b];

  auto fn = [&](const auto &a, const auto &b) -> StateValue {
    auto cmp = [&](const expr &a, const expr &b) {
      switch (cond) {
      case OEQ: return a.foeq(b);
      case OGT: return a.fogt(b);
      case OGE: return a.foge(b);
      case OLT: return a.folt(b);
      case OLE: return a.fole(b);
      case ONE: return a.fone(b);
      case ORD: return a.ford(b);
      case UEQ: return a.fueq(b);
      case UGT: return a.fugt(b);
      case UGE: return a.fuge(b);
      case ULT: return a.fult(b);
      case ULE: return a.fule(b);
      case UNE: return a.fune(b);
      case UNO: return a.funo(b);
      case TRUE:  return expr(true);
      case FALSE: return expr(false);
      }
    };
    auto [val, np] = fm_poison(s, a.value, a.non_poison, b.value, b.non_poison,
                               cmp, getType(), fmath, true, true);
    return { val.toBVBool(), std::move(np) };
  };

  if (auto agg = a->getType().getAsAggregateType()) {
    vector<StateValue> vals;
    for (unsigned i = 0, e = agg->numElementsConst(); i != e; ++i) {
      vals.emplace_back(fn(agg->extract(a_eval, i), agg->extract(b_eval, i)));
    }
    return getType().getAsAggregateType()->aggregateVals(vals);
  }
  return fn(a_eval, b_eval);
}

expr FCmp::getTypeConstraints(const Function &f) const {
  return Value::getTypeConstraints() &&
         getType().enforceIntOrVectorType(1) &&
         getType().enforceVectorTypeEquiv(a->getType()) &&
         a->getType().enforceFloatOrVectorType() &&
         a->getType() == b->getType();
}

unique_ptr<Instr> FCmp::dup(Function &f, const string &suffix) const {
  return make_unique<FCmp>(getType(), getName() + suffix, cond, *a, *b, fmath);
}


vector<Value*> Freeze::operands() const {
  return { val };
}

void Freeze::rauw(const Value &what, Value &with) {
  RAUW(val);
}

void Freeze::print(ostream &os) const {
  os << getName() << " = freeze " << print_type(getType()) << val->getName();
}

static StateValue freeze_elems(State &s, const Type &ty, const StateValue &v) {
  if (auto agg = ty.getAsAggregateType()) {
    vector<StateValue> vals;
    for (unsigned i = 0, e = agg->numElementsConst(); i != e; ++i) {
      if (agg->isPadding(i))
        continue;
      vals.emplace_back(freeze_elems(s, agg->getChild(i), agg->extract(v, i)));
    }
    return agg->aggregateVals(vals);
  }

  if (v.non_poison.isTrue())
    return v;

  StateValue ret_type = ty.getDummyValue(true);
  expr nondet = expr::mkFreshVar("nondet", ret_type.value);
  s.addQuantVar(nondet);
  return { expr::mkIf(v.non_poison, v.value, nondet),
           std::move(ret_type.non_poison) };
}

StateValue Freeze::toSMT(State &s) const {
  auto &v = s[*val];
  s.resetUndefVars();
  return freeze_elems(s, getType(), v);
}

expr Freeze::getTypeConstraints(const Function &f) const {
  return Value::getTypeConstraints() &&
         getType() == val->getType();
}

unique_ptr<Instr> Freeze::dup(Function &f, const string &suffix) const {
  return make_unique<Freeze>(getType(), getName() + suffix, *val);
}


void Phi::addValue(Value &val, string &&BB_name) {
  values.emplace_back(&val, std::move(BB_name));
}

void Phi::removeValue(const string &BB_name) {
  for (auto I = values.begin(), E = values.end(); I != E; ++I) {
    if (I->second == BB_name) {
      values.erase(I);
      break;
    }
  }
}

vector<string> Phi::sources() const {
  vector<string> s;
  for (auto &[_, bb] : values) {
    s.emplace_back(bb);
  }
  return s;
}

void Phi::replaceSourceWith(const string &from, const string &to) {
  for (auto &[_, bb] : values) {
    if (bb == from) {
      bb = to;
      break;
    }
  }
}

vector<Value*> Phi::operands() const {
  vector<Value*> v;
  for (auto &[val, bb] : values) {
    v.emplace_back(val);
  }
  return v;
}

void Phi::rauw(const Value &what, Value &with) {
  for (auto &[val, bb] : values) {
    RAUW(val);
  }
}

void Phi::replace(const string &predecessor, Value &newval) {
  for (auto &[val, bb] : values) {
    if (bb == predecessor) {
      val = &newval;
      break;
    }
  }
}

void Phi::print(ostream &os) const {
  os << getName() << " = phi " << fmath << print_type(getType());

  bool first = true;
  for (auto &[val, bb] : values) {
    if (!first)
      os << ", ";
    os << "[ " << val->getName() << ", " << bb << " ]";
    first = false;
  }
}

StateValue Phi::toSMT(State &s) const {
  DisjointExpr<StateValue> ret(getType().getDummyValue(false));
  map<Value*, StateValue> cache;

  for (auto &[val, bb] : values) {
    // check if this was a jump from unreachable BB
    if (auto pre = s.jumpCondFrom(s.getFn().getBB(bb))) {
      auto [I, inserted] = cache.try_emplace(val);
      if (inserted)
        I->second = s[*val];
      ret.add(I->second, (*pre)());
    }
  }

  StateValue sv = *std::move(ret)();
  auto identity = [](const expr &x) { return x; };
  return fm_poison(s, sv.value, sv.non_poison, identity, getType(), fmath, true,
                   false);
}

expr Phi::getTypeConstraints(const Function &f) const {
  auto c = Value::getTypeConstraints();
  for (auto &[val, bb] : values) {
    c &= val->getType() == getType();
  }
  return c;
}

unique_ptr<Instr> Phi::dup(Function &f, const string &suffix) const {
  auto phi = make_unique<Phi>(getType(), getName() + suffix);
  for (auto &[val, bb] : values) {
    phi->addValue(*val, string(bb));
  }
  return phi;
}


const BasicBlock& JumpInstr::target_iterator::operator*() const {
  if (auto br = dynamic_cast<const Branch*>(instr))
    return idx == 0 ? br->getTrue() : *br->getFalse();

  if (auto sw = dynamic_cast<const Switch*>(instr))
    return idx == 0 ? *sw->getDefault() : *sw->getTarget(idx-1).second;

  UNREACHABLE();
}

JumpInstr::target_iterator JumpInstr::it_helper::end() const {
  unsigned idx;
  if (!instr) {
    idx = 0;
  } else if (auto br = dynamic_cast<const Branch*>(instr)) {
    idx = br->getFalse() ? 2 : 1;
  } else if (auto sw = dynamic_cast<const Switch*>(instr)) {
    idx = sw->getNumTargets() + 1;
  } else {
    UNREACHABLE();
  }
  return { instr, idx };
}


void Branch::replaceTargetWith(const BasicBlock *from, const BasicBlock *to) {
  if (dst_true == from)
    dst_true = to;
  if (dst_false == from)
    dst_false = to;
}

vector<Value*> Branch::operands() const {
  if (cond)
    return { cond };
  return {};
}

void Branch::rauw(const Value &what, Value &with) {
  RAUW(cond);
}

void Branch::print(ostream &os) const {
  os << "br ";
  if (cond)
    os << *cond << ", ";
  os << "label " << dst_true->getName();
  if (dst_false)
    os << ", label " << dst_false->getName();
}

StateValue Branch::toSMT(State &s) const {
  if (cond) {
    auto &c = s.getAndAddPoisonUB(*cond, true);
    s.addCondJump(c.value, *dst_true, *dst_false);
  } else {
    s.addJump(*dst_true);
  }
  return {};
}

expr Branch::getTypeConstraints(const Function &f) const {
  if (!cond)
    return true;
  return cond->getType().enforceIntType(1);
}

unique_ptr<Instr> Branch::dup(Function &f, const string &suffix) const {
  if (dst_false)
    return make_unique<Branch>(*cond, *dst_true, *dst_false);
  return make_unique<Branch>(*dst_true);
}


void Switch::addTarget(Value &val, const BasicBlock &target) {
  targets.emplace_back(&val, &target);
}

void Switch::replaceTargetWith(const BasicBlock *from, const BasicBlock *to) {
  if (default_target == from)
    default_target = to;

  for (auto &[_, bb] : targets) {
    if (bb == from)
      bb = to;
  }
}

vector<Value*> Switch::operands() const {
  vector<Value*> ret = { value };
  for (auto &[val, target] : targets) {
    ret.emplace_back(val);
  }
  return ret;
}

void Switch::rauw(const Value &what, Value &with) {
  RAUW(value);
  for (auto &[val, target] : targets) {
    RAUW(val);
  }
}

void Switch::print(ostream &os) const {
  os << "switch " << *value << ", label " << default_target->getName() << " [\n";
  for (auto &[val, target] : targets) {
    os << "    " << *val << ", label " << target->getName() << '\n';
  }
  os << "  ]";
}

StateValue Switch::toSMT(State &s) const {
  auto &val = s.getAndAddPoisonUB(*value, true);
  expr default_cond(true);

  for (auto &[value_cond, bb] : targets) {
    auto &target = s[*value_cond];
    assert(target.non_poison.isTrue());
    expr cmp = val.value == target.value;
    default_cond &= !cmp;
    s.addJump(std::move(cmp), *bb);
  }

  s.addJump(std::move(default_cond), *default_target);
  s.addUB(expr(false));
  return {};
}

expr Switch::getTypeConstraints(const Function &f) const {
  expr typ = value->getType().enforceIntType();
  for (auto &p : targets) {
    typ &= p.first->getType() == value->getType();
  }
  return typ;
}

unique_ptr<Instr> Switch::dup(Function &f, const string &suffix) const {
  auto sw = make_unique<Switch>(*value, *default_target);
  for (auto &[value_cond, bb] : targets) {
    sw->addTarget(*value_cond, *bb);
  }
  return sw;
}


vector<Value*> Return::operands() const {
  return { val };
}

void Return::rauw(const Value &what, Value &with) {
  RAUW(val);
}

void Return::print(ostream &os) const {
  os << "ret ";
  if (!isVoid())
    os << print_type(getType());
  os << val->getName();
}

static StateValue
check_ret_attributes(State &s, StateValue &&sv, const Type &t,
                     const FnAttrs &attrs,
                     const vector<pair<Value*, ParamAttrs>> &args) {
  if (auto agg = t.getAsAggregateType()) {
    vector<StateValue> vals;
    for (unsigned i = 0, e = agg->numElementsConst(); i != e; ++i) {
      if (agg->isPadding(i))
        continue;
      vals.emplace_back(check_ret_attributes(s, agg->extract(sv, i),
                                             agg->getChild(i), attrs, args));
    }
    return agg->aggregateVals(vals);
  }

  if (t.isPtrType()) {
    Pointer p(s.getMemory(), sv.value);
    sv.non_poison &= !p.isStackAllocated();
    sv.non_poison &= !p.isNocapture();
  }

  return check_return_value(s, std::move(sv), t, attrs, args);
}

static void eq_val_rec(State &s, const Type &t, const StateValue &a,
                       const StateValue &b) {
  if (auto agg = t.getAsAggregateType()) {
    for (unsigned i = 0, e = agg->numElementsConst(); i != e; ++i) {
      if (agg->isPadding(i))
        continue;
      eq_val_rec(s, agg->getChild(i), agg->extract(a, i), agg->extract(b, i));
    }
    return;
  }
  s.addUB(a == b);
}

StateValue Return::toSMT(State &s) const {
  StateValue retval;

  auto &attrs = s.getFn().getFnAttrs();
  if (attrs.poisonImpliesUB())
    retval = s.getAndAddPoisonUB(*val, attrs.undefImpliesUB());
  else
    retval = s[*val];

  s.addUB(s.getMemory().checkNocapture());

  vector<pair<Value*, ParamAttrs>> args;
  for (auto &arg : s.getFn().getInputs()) {
    args.emplace_back(const_cast<Value*>(&arg), ParamAttrs());
  }

  retval = check_ret_attributes(s, std::move(retval), getType(), attrs, args);

  if (attrs.has(FnAttrs::NoReturn))
    s.addUB(expr(false));

  if (auto &val_returned = s.getReturnedInput())
    eq_val_rec(s, getType(), retval, *val_returned);

  s.addReturn(std::move(retval));
  return {};
}

expr Return::getTypeConstraints(const Function &f) const {
  return Value::getTypeConstraints() &&
         getType() == val->getType() &&
         f.getType() == getType();
}

unique_ptr<Instr> Return::dup(Function &f, const string &suffix) const {
  return make_unique<Return>(getType(), *val);
}


Assume::Assume(Value &cond, Kind kind)
    : Instr(Type::voidTy, "assume"), args({&cond}), kind(kind) {
  assert(kind == AndNonPoison || kind == IfNonPoison || kind == WellDefined ||
         kind == NonNull);
}

Assume::Assume(vector<Value *> &&args0, Kind kind)
    : Instr(Type::voidTy, "assume"), args(std::move(args0)), kind(kind) {
  if (args.size() == 1)
    assert(kind == AndNonPoison || kind == IfNonPoison || kind == WellDefined ||
           kind == NonNull);
  else {
    assert(kind == Align && args.size() == 2);
  }
}

vector<Value*> Assume::operands() const {
  return args;
}

void Assume::rauw(const Value &what, Value &with) {
  for (auto &arg: args)
    RAUW(arg);
}

void Assume::print(ostream &os) const {
  const char *str = nullptr;
  switch (kind) {
  case AndNonPoison: str = "assume "; break;
  case IfNonPoison:  str = "assume_non_poison "; break;
  case WellDefined:  str = "assume_welldefined "; break;
  case Align:        str = "assume_align "; break;
  case NonNull:      str = "assume_nonnull "; break;
  }
  os << str;

  bool first = true;
  for (auto &arg: args) {
    if (!first)
      os << ", ";
    os << *arg;
    first = false;
  }
}

StateValue Assume::toSMT(State &s) const {
  switch (kind) {
  case AndNonPoison: {
    auto &v = s.getAndAddPoisonUB(*args[0]);
    s.addUB(v.value != 0);
    break;
  }
  case IfNonPoison: {
    auto &[v, np] = s[*args[0]];
    s.addUB(np.implies(v != 0));
    break;
  }
  case WellDefined:
    (void)s.getAndAddPoisonUB(*args[0], true);
    break;
  case Align: {
    // assume(ptr, align)
    const auto &vptr = s.getAndAddPoisonUB(*args[0]);
    if (auto align = dynamic_cast<IntConst *>(args[1])) {
      Pointer ptr(s.getMemory(), vptr.value);
      s.addUB(ptr.isAligned(*align->getInt()));
    } else {
      // TODO: add support for non-constant align
      s.addUB(expr());
    }
    break;
  }
  case NonNull: {
    // assume(ptr)
    const auto &vptr = s.getAndAddPoisonUB(*args[0]);
    Pointer ptr(s.getMemory(), vptr.value);
    s.addUB(!ptr.isNull());
    break;
  }
  }
  return {};
}

expr Assume::getTypeConstraints(const Function &f) const {
  switch (kind) {
  case WellDefined:
    return true;
  case AndNonPoison:
  case IfNonPoison:
    return args[0]->getType().enforceIntType();
  case Align:
    return args[0]->getType().enforcePtrType() &&
           args[1]->getType().enforceIntType();
  case NonNull:
    return args[0]->getType().enforcePtrType();
  }
  return {};
}

unique_ptr<Instr> Assume::dup(Function &f, const string &suffix) const {
  return make_unique<Assume>(vector<Value *>(args), kind);
}


MemInstr::ByteAccessInfo MemInstr::ByteAccessInfo::intOnly(unsigned bytesz) {
  ByteAccessInfo info;
  info.byteSize = bytesz;
  info.hasIntByteAccess = true;
  return info;
}

MemInstr::ByteAccessInfo MemInstr::ByteAccessInfo::anyType(unsigned bytesz) {
  ByteAccessInfo info;
  info.byteSize = bytesz;
  return info;
}

MemInstr::ByteAccessInfo
MemInstr::ByteAccessInfo::get(const Type &t, bool store, unsigned align) {
  bool ptr_access = hasPtr(t);
  ByteAccessInfo info;
  info.hasIntByteAccess = t.enforcePtrOrVectorType().isFalse();
  info.doesPtrStore     = ptr_access && store;
  info.doesPtrLoad      = ptr_access && !store;
  info.byteSize         = gcd(align, getCommonAccessSize(t));
  return info;
}

MemInstr::ByteAccessInfo MemInstr::ByteAccessInfo::full(unsigned byteSize) {
  return { true, true, true, true, byteSize };
}


DEFINE_AS_RETZERO(Alloc, getMaxAccessSize);
DEFINE_AS_RETZERO(Alloc, getMaxGEPOffset);
DEFINE_AS_EMPTYACCESS(Alloc);

pair<uint64_t, uint64_t> Alloc::getMaxAllocSize() const {
  if (auto bytes = getInt(*size)) {
    if (*bytes && mul) {
      if (auto n = getInt(*mul))
        return { *n * abs(*bytes), align };
      return { UINT64_MAX, align };
    }
    return { *bytes, align };
  }
  return { UINT64_MAX, align };
}

vector<Value*> Alloc::operands() const {
  if (mul)
    return { size, mul };
  return { size };
}

void Alloc::rauw(const Value &what, Value &with) {
  RAUW(size);
  RAUW(mul);
}

void Alloc::print(ostream &os) const {
  os << getName() << " = alloca " << *size;
  if (mul)
    os << " x " << *mul;
  os << ", align " << align;
  if (initially_dead)
    os << ", dead";
}

StateValue Alloc::toSMT(State &s) const {
  auto sz = s.getAndAddPoisonUB(*size, true).value;

  if (mul) {
    auto &mul_e = s.getAndAddPoisonUB(*mul, true).value;

    if (sz.bits() > bits_size_t)
      s.addUB(mul_e == 0 || sz.extract(sz.bits()-1, bits_size_t) == 0);
    sz = sz.zextOrTrunc(bits_size_t);

    if (mul_e.bits() > bits_size_t)
      s.addUB(mul_e.extract(mul_e.bits()-1, bits_size_t) == 0);
    auto m = mul_e.zextOrTrunc(bits_size_t);

    s.addUB(sz.mul_no_uoverflow(m));
    sz = sz * m;
  }

  expr ptr = s.getMemory().alloc(sz, align, Memory::STACK, true, true).first;
  if (initially_dead)
    s.getMemory().free(ptr, true);
  return { std::move(ptr), true };
}

expr Alloc::getTypeConstraints(const Function &f) const {
  return Value::getTypeConstraints() &&
         getType().enforcePtrType() &&
         size->getType().enforceIntType();
}

unique_ptr<Instr> Alloc::dup(Function &f, const string &suffix) const {
  auto a = make_unique<Alloc>(getType(), getName() + suffix, *size, mul, align);
  if (initially_dead)
    a->markAsInitiallyDead();
  return a;
}


DEFINE_AS_RETZEROALIGN(StartLifetime, getMaxAllocSize);
DEFINE_AS_RETZERO(StartLifetime, getMaxAccessSize);
DEFINE_AS_RETZERO(StartLifetime, getMaxGEPOffset);
DEFINE_AS_EMPTYACCESS(StartLifetime);

vector<Value*> StartLifetime::operands() const {
  return { ptr };
}

void StartLifetime::rauw(const Value &what, Value &with) {
  RAUW(ptr);
}

void StartLifetime::print(ostream &os) const {
  os << "start_lifetime " << *ptr;
}

StateValue StartLifetime::toSMT(State &s) const {
  auto &p = s.getAndAddPoisonUB(*ptr, true).value;
  s.getMemory().startLifetime(p);
  return {};
}

expr StartLifetime::getTypeConstraints(const Function &f) const {
  return ptr->getType().enforcePtrType();
}

unique_ptr<Instr> StartLifetime::dup(Function &f, const string &suffix) const {
  return make_unique<StartLifetime>(*ptr);
}


DEFINE_AS_RETZEROALIGN(EndLifetime, getMaxAllocSize);
DEFINE_AS_RETZERO(EndLifetime, getMaxAccessSize);
DEFINE_AS_RETZERO(EndLifetime, getMaxGEPOffset);
DEFINE_AS_EMPTYACCESS(EndLifetime);

vector<Value*> EndLifetime::operands() const {
  return { ptr };
}

void EndLifetime::rauw(const Value &what, Value &with) {
  RAUW(ptr);
}

void EndLifetime::print(ostream &os) const {
  os << "start_lifetime " << *ptr;
}

StateValue EndLifetime::toSMT(State &s) const {
  auto &p = s.getAndAddPoisonUB(*ptr, true).value;
  s.getMemory().free(p, true);
  return {};
}

expr EndLifetime::getTypeConstraints(const Function &f) const {
  return ptr->getType().enforcePtrType();
}

unique_ptr<Instr> EndLifetime::dup(Function &f, const string &suffix) const {
  return make_unique<EndLifetime>(*ptr);
}


void GEP::addIdx(uint64_t obj_size, Value &idx) {
  idxs.emplace_back(obj_size, &idx);
}

DEFINE_AS_RETZEROALIGN(GEP, getMaxAllocSize);
DEFINE_AS_RETZERO(GEP, getMaxAccessSize);
DEFINE_AS_EMPTYACCESS(GEP);

static unsigned off_used_bits(const Value &v) {
  if (auto c = isCast(ConversionOp::SExt, v))
    return off_used_bits(c->getValue());

  if (auto ty = dynamic_cast<IntType*>(&v.getType()))
    return min(ty->bits(), 64u);

  return 64;
}

uint64_t GEP::getMaxGEPOffset() const {
  uint64_t off = 0;
  for (auto &[mul, v] : getIdxs()) {
    if (mul == 0)
      continue;
    if (mul >= INT64_MAX)
      return UINT64_MAX;

    if (auto n = getInt(*v)) {
      off = add_saturate(off, abs((int64_t)mul * *n));
      continue;
    }

    off = add_saturate(off,
                       mul_saturate(mul,
                                    UINT64_MAX >> (64 - off_used_bits(*v))));
  }
  return off;
}

vector<Value*> GEP::operands() const {
  vector<Value*> v = { ptr };
  for (auto &[sz, idx] : idxs) {
    v.emplace_back(idx);
  }
  return v;
}

void GEP::rauw(const Value &what, Value &with) {
  RAUW(ptr);
  for (auto &[sz, idx] : idxs) {
    RAUW(idx);
  }
}

void GEP::print(ostream &os) const {
  os << getName() << " = gep ";
  if (inbounds)
    os << "inbounds ";
  os << *ptr;

  for (auto &[sz, idx] : idxs) {
    os << ", " << sz << " x " << *idx;
  }
}

StateValue GEP::toSMT(State &s) const {
  auto scalar = [&](const StateValue &ptrval,
                    vector<pair<uint64_t, StateValue>> &offsets) -> StateValue {
    Pointer ptr(s.getMemory(), ptrval.value);
    AndExpr non_poison(ptrval.non_poison);

    if (inbounds)
      non_poison.add(ptr.inbounds(true));

    for (auto &[sz, idx] : offsets) {
      auto &[v, np] = idx;
      auto multiplier = expr::mkUInt(sz, bits_for_offset);
      auto val = v.sextOrTrunc(bits_for_offset);
      auto inc = multiplier * val;

      if (inbounds) {
        if (sz != 0)
          non_poison.add(val.sextOrTrunc(v.bits()) == v);
        non_poison.add(multiplier.mul_no_soverflow(val));
        non_poison.add(ptr.addNoOverflow(inc));
      }

#ifndef NDEBUG
      int64_t n;
      if (inc.isInt(n))
        assert(ilog2_ceil(abs(n), true) <= bits_for_offset);
#endif

      ptr += inc;
      non_poison.add(np);

      if (inbounds)
        non_poison.add(ptr.inbounds());
    }
    return { ptr.release(), non_poison() };
  };

  if (auto aty = getType().getAsAggregateType()) {
    vector<StateValue> vals;
    auto &ptrval = s[*ptr];
    bool ptr_isvect = ptr->getType().isVectorType();

    for (unsigned i = 0, e = aty->numElementsConst(); i != e; ++i) {
      vector<pair<uint64_t, StateValue>> offsets;
      for (auto &[sz, idx] : idxs) {
        if (auto idx_aty = idx->getType().getAsAggregateType())
          offsets.emplace_back(sz, idx_aty->extract(s[*idx], i));
        else
          offsets.emplace_back(sz, s[*idx]);
      }
      vals.emplace_back(scalar(ptr_isvect ? aty->extract(ptrval, i) :
                               (i == 0 ? ptrval : s[*ptr]), offsets));
    }
    return getType().getAsAggregateType()->aggregateVals(vals);
  }
  vector<pair<uint64_t, StateValue>> offsets;
  for (auto &[sz, idx] : idxs)
    offsets.emplace_back(sz, s[*idx]);
  return scalar(s[*ptr], offsets);
}

expr GEP::getTypeConstraints(const Function &f) const {
  auto c = Value::getTypeConstraints() &&
           getType().enforceVectorTypeIff(ptr->getType()) &&
           getType().enforcePtrOrVectorType();
  for (auto &[sz, idx] : idxs) {
    // It is allowed to have non-vector idx with vector pointer operand
    c &= idx->getType().enforceIntOrVectorType() &&
          getType().enforceVectorTypeIff(idx->getType());
  }
  return c;
}

unique_ptr<Instr> GEP::dup(Function &f, const string &suffix) const {
  auto dup = make_unique<GEP>(getType(), getName() + suffix, *ptr, inbounds);
  for (auto &[sz, idx] : idxs) {
    dup->addIdx(sz, *idx);
  }
  return dup;
}


DEFINE_AS_RETZEROALIGN(Load, getMaxAllocSize);
DEFINE_AS_RETZERO(Load, getMaxGEPOffset);

uint64_t Load::getMaxAccessSize() const {
  return Memory::getStoreByteSize(getType());
}

MemInstr::ByteAccessInfo Load::getByteAccessInfo() const {
  return ByteAccessInfo::get(getType(), false, align);
}

vector<Value*> Load::operands() const {
  return { ptr };
}

void Load::rauw(const Value &what, Value &with) {
  RAUW(ptr);
}

void Load::print(ostream &os) const {
  os << getName() << " = load " << getType() << ", " << *ptr
     << ", align " << align;
}

StateValue Load::toSMT(State &s) const {
  auto &p = s.getAndAddPoisonUB(*ptr, true).value;
  check_can_load(s, p);
  auto [sv, ub] = s.getMemory().load(p, getType(), align);
  s.addUB(std::move(ub));
  return sv;
}

expr Load::getTypeConstraints(const Function &f) const {
  return Value::getTypeConstraints() &&
         ptr->getType().enforcePtrType();
}

unique_ptr<Instr> Load::dup(Function &f, const string &suffix) const {
  return make_unique<Load>(getType(), getName() + suffix, *ptr, align);
}


DEFINE_AS_RETZEROALIGN(Store, getMaxAllocSize);
DEFINE_AS_RETZERO(Store, getMaxGEPOffset);

uint64_t Store::getMaxAccessSize() const {
  return Memory::getStoreByteSize(val->getType());
}

MemInstr::ByteAccessInfo Store::getByteAccessInfo() const {
  return ByteAccessInfo::get(val->getType(), true, align);
}

vector<Value*> Store::operands() const {
  return { val, ptr };
}

void Store::rauw(const Value &what, Value &with) {
  RAUW(val);
  RAUW(ptr);
}

void Store::print(ostream &os) const {
  os << "store " << *val << ", " << *ptr << ", align " << align;
}

StateValue Store::toSMT(State &s) const {
  // skip large initializers. FIXME: this should be moved to memory so it can
  // fold subsequent trivial loads
  if (s.isInitializationPhase() &&
      Memory::getStoreByteSize(val->getType()) / (bits_byte / 8) > 128) {
    s.doesApproximation("Large constant initializer removed");
    return {};
  }

  auto &p = s.getAndAddPoisonUB(*ptr, true).value;
  check_can_store(s, p);
  auto &v = s[*val];
  s.getMemory().store(p, v, val->getType(), align, s.getUndefVars());
  return {};
}

expr Store::getTypeConstraints(const Function &f) const {
  return ptr->getType().enforcePtrType();
}

unique_ptr<Instr> Store::dup(Function &f, const string &suffix) const {
  return make_unique<Store>(*ptr, *val, align);
}


DEFINE_AS_RETZEROALIGN(Memset, getMaxAllocSize);
DEFINE_AS_RETZERO(Memset, getMaxGEPOffset);

uint64_t Memset::getMaxAccessSize() const {
  return getIntOr(*bytes, UINT64_MAX);
}

MemInstr::ByteAccessInfo Memset::getByteAccessInfo() const {
  unsigned byteSize = 1;
  if (auto bs = getInt(*bytes))
    byteSize = gcd(align, *bs);
  return ByteAccessInfo::intOnly(byteSize);
}

vector<Value*> Memset::operands() const {
  return { ptr, val, bytes };
}

void Memset::rauw(const Value &what, Value &with) {
  RAUW(ptr);
  RAUW(val);
  RAUW(bytes);
}

void Memset::print(ostream &os) const {
  os << "memset " << *ptr << " align " << align << ", " << *val
     << ", " << *bytes;
}

StateValue Memset::toSMT(State &s) const {
  auto &vbytes = s.getAndAddPoisonUB(*bytes, true).value;

  uint64_t n;
  expr vptr;
  if (vbytes.isUInt(n) && n > 0) {
    vptr = s.getAndAddPoisonUB(*ptr, true).value;
  } else {
    auto &sv_ptr = s[*ptr];
    auto &sv_ptr2 = s[*ptr];
    // can't be poison even if bytes=0 as address must be aligned regardless
    s.addUB(sv_ptr.non_poison);
    s.addUB((vbytes != 0).implies(sv_ptr.value == sv_ptr2.value));
    vptr = sv_ptr.value;
  }
  check_can_store(s, vptr);
  s.getMemory().memset(vptr, s[*val].zextOrTrunc(8), vbytes, align,
                       s.getUndefVars());
  return {};
}

expr Memset::getTypeConstraints(const Function &f) const {
  return ptr->getType().enforcePtrType() &&
         val->getType().enforceIntType() &&
         bytes->getType().enforceIntType();
}

unique_ptr<Instr> Memset::dup(Function &f, const string &suffix) const {
  return make_unique<Memset>(*ptr, *val, *bytes, align);
}


DEFINE_AS_RETZEROALIGN(MemsetPattern, getMaxAllocSize);
DEFINE_AS_RETZERO(MemsetPattern, getMaxGEPOffset);

MemsetPattern::MemsetPattern(Value &ptr, Value &pattern, Value &bytes,
                             unsigned pattern_length)
  : MemInstr(Type::voidTy, "memset_pattern" + to_string(pattern_length)),
    ptr(&ptr), pattern(&pattern), bytes(&bytes),
    pattern_length(pattern_length) {}

uint64_t MemsetPattern::getMaxAccessSize() const {
  return getIntOr(*bytes, UINT64_MAX);
}

MemInstr::ByteAccessInfo MemsetPattern::getByteAccessInfo() const {
  unsigned byteSize = 1;
  if (auto bs = getInt(*bytes))
    byteSize = *bs;
  return ByteAccessInfo::intOnly(byteSize);
}

vector<Value*> MemsetPattern::operands() const {
  return { ptr, pattern, bytes };
}

void MemsetPattern::rauw(const Value &what, Value &with) {
  RAUW(ptr);
  RAUW(pattern);
  RAUW(bytes);
}

void MemsetPattern::print(ostream &os) const {
  os << getName() << ' ' << *ptr << ", " << *pattern << ", " << *bytes;
}

StateValue MemsetPattern::toSMT(State &s) const {
  auto &vptr = s.getAndAddPoisonUB(*ptr, false).value;
  auto &vpattern = s.getAndAddPoisonUB(*pattern, false).value;
  auto &vbytes = s.getAndAddPoisonUB(*bytes, true).value;
  check_can_store(s, vptr);
  check_can_load(s, vpattern);
  s.getMemory().memset_pattern(vptr, vpattern, vbytes, pattern_length);
  return {};
}

expr MemsetPattern::getTypeConstraints(const Function &f) const {
  return ptr->getType().enforcePtrType() &&
         pattern->getType().enforcePtrType() &&
         bytes->getType().enforceIntType();
}

unique_ptr<Instr> MemsetPattern::dup(Function &f, const string &suffix) const {
  return make_unique<MemsetPattern>(*ptr, *pattern, *bytes, pattern_length);
}


DEFINE_AS_RETZEROALIGN(FillPoison, getMaxAllocSize);
DEFINE_AS_RETZERO(FillPoison, getMaxGEPOffset);

uint64_t FillPoison::getMaxAccessSize() const {
  return getGlobalVarSize(ptr);
}

MemInstr::ByteAccessInfo FillPoison::getByteAccessInfo() const {
  return ByteAccessInfo::intOnly(1);
}

vector<Value*> FillPoison::operands() const {
  return { ptr };
}

void FillPoison::rauw(const Value &what, Value &with) {
  RAUW(ptr);
}

void FillPoison::print(ostream &os) const {
  os << "fillpoison " << *ptr;
}

StateValue FillPoison::toSMT(State &s) const {
  auto &vptr = s.getAndAddPoisonUB(*ptr, true).value;
  Memory &m = s.getMemory();
  m.fillPoison(Pointer(m, vptr).getBid());
  return {};
}

expr FillPoison::getTypeConstraints(const Function &f) const {
  return ptr->getType().enforcePtrType();
}

unique_ptr<Instr> FillPoison::dup(Function &f, const string &suffix) const {
  return make_unique<FillPoison>(*ptr);
}


DEFINE_AS_RETZEROALIGN(Memcpy, getMaxAllocSize);
DEFINE_AS_RETZERO(Memcpy, getMaxGEPOffset);

uint64_t Memcpy::getMaxAccessSize() const {
  return getIntOr(*bytes, UINT64_MAX);
}

MemInstr::ByteAccessInfo Memcpy::getByteAccessInfo() const {
#if 0
  if (auto bytes = get_int(i->getBytes()))
    byteSize = gcd(gcd(i->getSrcAlign(), i->getDstAlign()), *bytes);
#endif
  // FIXME: memcpy doesn't have multi-byte support
  // Memcpy does not have sub-byte access, unless the sub-byte type appears
  // at other instructions
  auto info = ByteAccessInfo::full(1);
  info.observesAddresses = false;
  return info;
}

vector<Value*> Memcpy::operands() const {
  return { dst, src, bytes };
}

void Memcpy::rauw(const Value &what, Value &with) {
  RAUW(dst);
  RAUW(src);
  RAUW(bytes);
}

void Memcpy::print(ostream &os) const {
  os << (move ? "memmove " : "memcpy ") << *dst  << " align " << align_dst
     << ", " << *src << " align " << align_src << ", " << *bytes;
}

StateValue Memcpy::toSMT(State &s) const {
  auto &vbytes = s.getAndAddPoisonUB(*bytes, true).value;

  uint64_t n;
  expr vsrc, vdst;
  if (align_dst || (vbytes.isUInt(n) && n > 0)) {
    vdst = s.getAndAddPoisonUB(*dst, true).value;
  } else {
    auto &sv_dst = s[*dst];
    auto &sv_dst2 = s[*dst];
    s.addUB((vbytes != 0).implies(
              sv_dst.non_poison && sv_dst.value == sv_dst2.value));
    vdst = sv_dst.value;
  }

  if (align_src || (vbytes.isUInt(n) && n > 0)) {
    vsrc = s.getAndAddPoisonUB(*src, true).value;
  } else {
    auto &sv_src = s[*src];
    auto &sv_src2 = s[*src];
    s.addUB((vbytes != 0).implies(
               sv_src.non_poison && sv_src.value == sv_src2.value));
    vsrc = sv_src.value;
  }

  if (vbytes.bits() > bits_size_t)
    s.addUB(
      vbytes.ule(expr::IntUMax(bits_size_t).zext(vbytes.bits() - bits_size_t)));

  check_can_load(s, vsrc);
  check_can_store(s, vdst);
  s.getMemory().memcpy(vdst, vsrc, vbytes, align_dst, align_src, move);
  return {};
}

expr Memcpy::getTypeConstraints(const Function &f) const {
  return dst->getType().enforcePtrType() &&
         dst->getType().enforcePtrType() &&
         bytes->getType().enforceIntType();
}

unique_ptr<Instr> Memcpy::dup(Function &f, const string &suffix) const {
  return make_unique<Memcpy>(*dst, *src, *bytes, align_dst, align_src, move);
}



DEFINE_AS_RETZEROALIGN(Memcmp, getMaxAllocSize);
DEFINE_AS_RETZERO(Memcmp, getMaxGEPOffset);

uint64_t Memcmp::getMaxAccessSize() const {
  return getIntOr(*num, UINT64_MAX);
}

MemInstr::ByteAccessInfo Memcmp::getByteAccessInfo() const {
  auto info = ByteAccessInfo::anyType(1);
  info.observesAddresses = true;
  return info;
}

vector<Value*> Memcmp::operands() const {
  return { ptr1, ptr2, num };
}

void Memcmp::rauw(const Value &what, Value &with) {
  RAUW(ptr1);
  RAUW(ptr2);
  RAUW(num);
}

void Memcmp::print(ostream &os) const {
  os << getName() << " = " << (is_bcmp ? "bcmp " : "memcmp ") << *ptr1
     << ", " << *ptr2 << ", " << *num;
}

StateValue Memcmp::toSMT(State &s) const {
  auto &[vptr1, np1] = s[*ptr1];
  auto &[vptr2, np2] = s[*ptr2];
  auto &vnum = s.getAndAddPoisonUB(*num).value;
  s.addUB((vnum != 0).implies(np1 && np2));

  check_can_load(s, vptr1);
  check_can_load(s, vptr2);

  Pointer p1(s.getMemory(), vptr1), p2(s.getMemory(), vptr2);
  // memcmp can be optimized to load & icmps, and it requires this
  // dereferenceability check of vnum.
  s.addUB(p1.isDereferenceable(vnum, 1, false));
  s.addUB(p2.isDereferenceable(vnum, 1, false));

  expr zero = expr::mkUInt(0, 32);

  expr result_var, result_var_neg;
  if (is_bcmp) {
    result_var = expr::mkFreshVar("bcmp_nonzero", zero);
    s.addPre(result_var != zero);
    s.addQuantVar(result_var);
  } else {
    auto z31 = expr::mkUInt(0, 31);
    result_var = expr::mkFreshVar("memcmp_nonzero", z31);
    s.addPre(result_var != z31);
    s.addQuantVar(result_var);
    result_var = expr::mkUInt(0, 1).concat(result_var);

    result_var_neg = expr::mkFreshVar("memcmp", z31);
    s.addQuantVar(result_var_neg);
    result_var_neg = expr::mkUInt(1, 1).concat(result_var_neg);
  }

  auto ith_exec =
      [&, this](unsigned i, bool is_last) -> tuple<expr, expr, AndExpr, expr> {
    assert(bits_byte == 8); // TODO: remove constraint
    auto val1 = s.getMemory().raw_load(p1 + i);
    auto val2 = s.getMemory().raw_load(p2 + i);
    expr is_ptr1 = val1.isPtr();
    expr is_ptr2 = val2.isPtr();

    expr result_neq;
    if (is_bcmp) {
      result_neq = result_var;
    } else {
      expr pos
        = mkIf_fold(is_ptr1,
                    val1.ptr().getAddress().uge(val2.ptr().getAddress()),
                    val1.nonptrValue().uge(val2.nonptrValue()));
      result_neq = expr::mkIf(pos, result_var, result_var_neg);
    }

    // allow null <-> 0 comparison
    expr val_eq =
      (is_ptr1 == is_ptr2 &&
       mkIf_fold(is_ptr1,
                 val1.ptr().getAddress() == val2.ptr().getAddress(),
                 val1.nonptrValue() == val2.nonptrValue())) ||
      (val1.isZero() && val2.isZero());

    expr np
      = (is_ptr1 == is_ptr2 || val1.isZero() || val2.isZero()) &&
        !val1.isPoison() && !val2.isPoison();

    return { expr::mkIf(val_eq, zero, result_neq),
             std::move(np), {},
             val_eq && vnum.uge(i + 2) };
  };
  auto [val, np, ub]
    = LoopLikeFunctionApproximator(ith_exec).encode(s, memcmp_unroll_cnt);
  return { expr::mkIf(vnum == 0, zero, std::move(val)), (vnum != 0).implies(np) };
}

expr Memcmp::getTypeConstraints(const Function &f) const {
  return ptr1->getType().enforcePtrType() &&
         ptr2->getType().enforcePtrType() &&
         num->getType().enforceIntType();
}

unique_ptr<Instr> Memcmp::dup(Function &f, const string &suffix) const {
  return make_unique<Memcmp>(getType(), getName() + suffix, *ptr1, *ptr2, *num,
                             is_bcmp);
}


DEFINE_AS_RETZEROALIGN(Strlen, getMaxAllocSize);
DEFINE_AS_RETZERO(Strlen, getMaxGEPOffset);

uint64_t Strlen::getMaxAccessSize() const {
  return getGlobalVarSize(ptr);
}

MemInstr::ByteAccessInfo Strlen::getByteAccessInfo() const {
  return ByteAccessInfo::intOnly(1); /* strlen raises UB on ptr bytes */
}

vector<Value*> Strlen::operands() const {
  return { ptr };
}

void Strlen::rauw(const Value &what, Value &with) {
  RAUW(ptr);
}

void Strlen::print(ostream &os) const {
  os << getName() << " = strlen " << *ptr;
}

StateValue Strlen::toSMT(State &s) const {
  auto &eptr = s.getAndAddPoisonUB(*ptr, true).value;
  check_can_load(s, eptr);

  Pointer p(s.getMemory(), eptr);
  Type &ty = getType();

  auto ith_exec =
      [&s, &p, &ty](unsigned i, bool _) -> tuple<expr, expr, AndExpr, expr> {
    AndExpr ub;
    auto [val, ub_load] = s.getMemory().load((p + i)(), IntType("i8", 8), 1);
    ub.add(std::move(ub_load));
    ub.add(std::move(val.non_poison));
    return { expr::mkUInt(i, ty.bits()), true, std::move(ub), val.value != 0 };
  };
  auto [val, _, ub]
    = LoopLikeFunctionApproximator(ith_exec).encode(s, strlen_unroll_cnt);
  s.addUB(std::move(ub));
  return { std::move(val), true };
}

expr Strlen::getTypeConstraints(const Function &f) const {
  return Value::getTypeConstraints() &&
         ptr->getType().enforcePtrType() &&
         getType().enforceIntType();
}

unique_ptr<Instr> Strlen::dup(Function &f, const string &suffix) const {
  return make_unique<Strlen>(getType(), getName() + suffix, *ptr);
}


vector<Value*> VaStart::operands() const {
  return { ptr };
}

void VaStart::rauw(const Value &what, Value &with) {
  RAUW(ptr);
}

void VaStart::print(ostream &os) const {
  os << "call void @llvm.va_start(" << *ptr << ')';
}

StateValue VaStart::toSMT(State &s) const {
  s.addUB(expr(s.getFn().isVarArgs()));

  auto &data  = s.getVarArgsData();
  auto &raw_p = s.getAndAddPoisonUB(*ptr, true).value;

  expr zero     = expr::mkUInt(0, VARARG_BITS);
  expr num_args = expr::mkVar("num_va_args", VARARG_BITS);

  // just in case there's already a pointer there
  OrExpr matched_one;
  for (auto &[ptr, entry] : data) {
    // FIXME. if entry.alive => memory leak (though not UB). detect this
    expr eq = ptr == raw_p;
    entry.alive      |= eq;
    entry.next_arg    = expr::mkIf(eq, zero, entry.next_arg);
    entry.num_args    = expr::mkIf(eq, num_args, entry.num_args);
    entry.is_va_start = expr::mkIf(eq, true, entry.is_va_start);
    matched_one.add(std::move(eq));
  }

  Pointer ptr(s.getMemory(), raw_p);
  s.addUB(ptr.isBlockAlive());
  s.addUB(ptr.blockSize().uge(4)); // FIXME: this is target dependent

  // alive, next_arg, num_args, is_va_start, active
  data.try_emplace(raw_p, expr(true), std::move(zero), std::move(num_args),
                   expr(true), !matched_one());

  return {};
}

expr VaStart::getTypeConstraints(const Function &f) const {
  return ptr->getType().enforcePtrType();
}

unique_ptr<Instr> VaStart::dup(Function &f, const string &suffix) const {
  return make_unique<VaStart>(*ptr);
}


vector<Value*> VaEnd::operands() const {
  return { ptr };
}

void VaEnd::rauw(const Value &what, Value &with) {
  RAUW(ptr);
}

void VaEnd::print(ostream &os) const {
  os << "call void @llvm.va_end(" << *ptr << ')';
}

template <typename D>
static void ensure_varargs_ptr(D &data, State &s, const expr &arg_ptr) {
  OrExpr matched_one;
  for (auto &[ptr, entry] : data) {
    matched_one.add(ptr == arg_ptr);
  }

  expr matched = matched_one();
  if (matched.isTrue())
    return;

  // Insert a new entry in case there was none before.
  // This might be a ptr passed as argument (va_start in the callee).
  s.addUB(matched || !Pointer(s.getMemory(), arg_ptr).isLocal());

  expr zero = expr::mkUInt(0, VARARG_BITS);
  ENSURE(data.try_emplace(arg_ptr,
                          expr::mkUF("vararg_alive", { arg_ptr }, false),
                          expr(zero), // = next_arg
                          expr::mkUF("vararg_num_args", { arg_ptr }, zero),
                          expr(false), // = is_va_start
                          !matched).second);
}

StateValue VaEnd::toSMT(State &s) const {
  auto &data  = s.getVarArgsData();
  auto &raw_p = s.getAndAddPoisonUB(*ptr, true).value;

  s.addUB(Pointer(s.getMemory(), raw_p).isBlockAlive());

  ensure_varargs_ptr(data, s, raw_p);

  for (auto &[ptr, entry] : data) {
    expr eq = ptr == raw_p;
    s.addUB((eq && entry.active).implies(entry.alive));
    entry.alive &= !eq;
  }
  return {};
}

expr VaEnd::getTypeConstraints(const Function &f) const {
  return ptr->getType().enforcePtrType();
}

unique_ptr<Instr> VaEnd::dup(Function &f, const string &suffix) const {
  return make_unique<VaEnd>(*ptr);
}


vector<Value*> VaCopy::operands() const {
  return { dst, src };
}

void VaCopy::rauw(const Value &what, Value &with) {
  RAUW(dst);
  RAUW(src);
}

void VaCopy::print(ostream &os) const {
  os << "call void @llvm.va_copy(" << *dst << ", " << *src << ')';
}

StateValue VaCopy::toSMT(State &s) const {
  auto &data = s.getVarArgsData();
  auto &dst_raw = s.getAndAddPoisonUB(*dst, true).value;
  auto &src_raw = s.getAndAddPoisonUB(*src, true).value;
  Pointer dst(s.getMemory(), dst_raw);
  Pointer src(s.getMemory(), src_raw);

  s.addUB(dst.isBlockAlive());
  s.addUB(src.isBlockAlive());
  s.addUB(dst.blockSize() == src.blockSize());

  ensure_varargs_ptr(data, s, src_raw);

  DisjointExpr<expr> next_arg, num_args, is_va_start;
  for (auto &[ptr, entry] : data) {
    expr select = entry.active && ptr == src_raw;
    s.addUB(select.implies(entry.alive));

    next_arg.add(entry.next_arg, select);
    num_args.add(entry.num_args, select);
    is_va_start.add(entry.is_va_start, std::move(select));

    // kill aliases
    entry.active &= ptr != dst_raw;
  }

  // FIXME: dst should be empty or we have a mem leak
  // alive, next_arg, num_args, is_va_start, active
  data[dst_raw] = { true, *std::move(next_arg)(), *std::move(num_args)(),
                    *std::move(is_va_start)(), true };

  return {};
}

expr VaCopy::getTypeConstraints(const Function &f) const {
  return dst->getType().enforcePtrType() &&
         src->getType().enforcePtrType();
}

unique_ptr<Instr> VaCopy::dup(Function &f, const string &suffix) const {
  return make_unique<VaCopy>(*dst, *src);
}


vector<Value*> VaArg::operands() const {
  return { ptr };
}

void VaArg::rauw(const Value &what, Value &with) {
  RAUW(ptr);
}

void VaArg::print(ostream &os) const {
  os << getName() << " = va_arg " << *ptr << ", " << getType();
}

StateValue VaArg::toSMT(State &s) const {
  auto &data  = s.getVarArgsData();
  auto &raw_p = s.getAndAddPoisonUB(*ptr, true).value;

  s.addUB(Pointer(s.getMemory(), raw_p).isBlockAlive());

  ensure_varargs_ptr(data, s, raw_p);

  DisjointExpr<StateValue> ret(StateValue{});
  expr value_kind = getType().getDummyValue(false).value;
  expr one = expr::mkUInt(1, VARARG_BITS);

  for (auto &[ptr, entry] : data) {
    string type = getType().toString();
    string arg_name = "va_arg_" + type;
    string arg_in_name = "va_arg_in_" + type;
    StateValue val = {
      expr::mkIf(entry.is_va_start,
                 expr::mkUF(arg_name.c_str(), { entry.next_arg }, value_kind),
                 expr::mkUF(arg_in_name.c_str(), { ptr, entry.next_arg },
                            value_kind)),
      expr::mkIf(entry.is_va_start,
                 expr::mkUF("va_arg_np", { entry.next_arg }, true),
                 expr::mkUF("va_arg_np_in", { ptr, entry.next_arg }, true))
    };
    expr eq = ptr == raw_p;
    expr select = entry.active && eq;
    ret.add(std::move(val), select);

    expr next_arg = entry.next_arg + one;
    s.addUB(select.implies(entry.alive && entry.num_args.uge(next_arg)));
    entry.next_arg = expr::mkIf(eq, next_arg, entry.next_arg);
  }

  return *std::move(ret)();
}

expr VaArg::getTypeConstraints(const Function &f) const {
  return getType().enforceScalarType() &&
         ptr->getType().enforcePtrType();
}

unique_ptr<Instr> VaArg::dup(Function &f, const string &suffix) const {
  return make_unique<VaArg>(getType(), getName() + suffix, *ptr);
}


vector<Value*> ExtractElement::operands() const {
  return { v, idx };
}

void ExtractElement::rauw(const Value &what, Value &with) {
  RAUW(v);
  RAUW(idx);
}

void ExtractElement::print(ostream &os) const {
  os << getName() << " = extractelement " << *v << ", " << *idx;
}

StateValue ExtractElement::toSMT(State &s) const {
  auto &[iv, ip] = s[*idx];
  auto vty = static_cast<const VectorType*>(v->getType().getAsAggregateType());
  expr inbounds = iv.ult(vty->numElementsConst());
  auto [rv, rp] = vty->extract(s[*v], iv);
  return { std::move(rv), ip && inbounds && rp };
}

expr ExtractElement::getTypeConstraints(const Function &f) const {
  return Value::getTypeConstraints() &&
         v->getType().enforceVectorType([&](auto &ty)
                                        { return ty == getType(); }) &&
         idx->getType().enforceIntType();
}

unique_ptr<Instr> ExtractElement::dup(Function &f, const string &suffix) const {
  return make_unique<ExtractElement>(getType(), getName() + suffix, *v, *idx);
}


vector<Value*> InsertElement::operands() const {
  return { v, e, idx };
}

void InsertElement::rauw(const Value &what, Value &with) {
  RAUW(v);
  RAUW(e);
  RAUW(idx);
}

void InsertElement::print(ostream &os) const {
  os << getName() << " = insertelement " << *v << ", " << *e << ", " << *idx;
}

StateValue InsertElement::toSMT(State &s) const {
  auto &[iv, ip] = s[*idx];
  auto vty = static_cast<const VectorType*>(v->getType().getAsAggregateType());
  expr inbounds = iv.ult(vty->numElementsConst());
  auto [rv, rp] = vty->update(s[*v], s[*e], iv);
  return { std::move(rv), expr::mkIf(ip && inbounds, std::move(rp),
                                vty->getDummyValue(false).non_poison) };
}

expr InsertElement::getTypeConstraints(const Function &f) const {
  return Value::getTypeConstraints() &&
         getType() == v->getType() &&
         v->getType().enforceVectorType([&](auto &ty)
                                        { return ty == e->getType(); }) &&
         idx->getType().enforceIntType();
}

unique_ptr<Instr> InsertElement::dup(Function &f, const string &suffix) const {
  return make_unique<InsertElement>(getType(), getName() + suffix,
                                    *v, *e, *idx);
}


vector<Value*> ShuffleVector::operands() const {
  return { v1, v2 };
}

void ShuffleVector::rauw(const Value &what, Value &with) {
  RAUW(v1);
  RAUW(v2);
}

void ShuffleVector::print(ostream &os) const {
  os << getName() << " = shufflevector " << *v1 << ", " << *v2;
  for (auto m : mask)
    os << ", " << m;
}

StateValue ShuffleVector::toSMT(State &s) const {
  auto vty = v1->getType().getAsAggregateType();
  auto sz = vty->numElementsConst();
  vector<StateValue> vals;

  for (auto m : mask) {
    if (m >= 2 * sz) {
      vals.emplace_back(vty->getChild(0).getDummyValue(false));
    } else {
      auto *vect = &s[m < sz ? *v1 : *v2];
      vals.emplace_back(vty->extract(*vect, m % sz));
    }
  }

  return getType().getAsAggregateType()->aggregateVals(vals);
}

expr ShuffleVector::getTypeConstraints(const Function &f) const {
  return Value::getTypeConstraints() &&
         getType().enforceVectorTypeSameChildTy(v1->getType()) &&
         getType().getAsAggregateType()->numElements() == mask.size() &&
         v1->getType().enforceVectorType() &&
         v1->getType() == v2->getType();
}

unique_ptr<Instr> ShuffleVector::dup(Function &f, const string &suffix) const {
  return make_unique<ShuffleVector>(getType(), getName() + suffix,
                                    *v1, *v2, mask);
}


vector<Value*> FakeShuffle::operands() const {
  return { v1, v2, mask };
}

void FakeShuffle::rauw(const Value &what, Value &with) {
  RAUW(v1);
  RAUW(v2);
  RAUW(mask);
}

void FakeShuffle::print(ostream &os) const {
  os << getName() << " = fakesv " << *v1 << ", " << *v2;
}

StateValue FakeShuffle::toSMT(State &s) const {
  auto vty = static_cast<const VectorType*>(v1->getType().getAsAggregateType());
  auto mty = mask->getType().getAsAggregateType();
  auto sz = vty->numElementsConst();
  vector<StateValue> vals;

  for (unsigned i = 0, e = mty->numElementsConst(); i != e; ++i) {
    auto mi = mty->extract(s[*mask], i);
    auto idx = mi.value.urem(sz);
    auto [v1v, v1p] = vty->extract(s[*v1], idx);
    auto [v2v, v2p] = vty->extract(s[*v2], idx);
    expr v  = expr::mkIf(mi.value.ult(sz), v1v, v2v);
    expr np = expr::mkIf(mi.value.ult(sz), v1p, v2p);

    expr inbounds = mi.value.ult(vty->numElementsConst() * 2);

    vals.emplace_back(move(v), inbounds & np);
  }

  return getType().getAsAggregateType()->aggregateVals(vals);
}

expr FakeShuffle::getTypeConstraints(const Function &f) const {
  return Value::getTypeConstraints() &&
         getType().enforceVectorTypeSameChildTy(v1->getType()) &&
         getType().getAsAggregateType()->numElements() == mask->getType().getAsAggregateType()->numElements() &&
         v1->getType().enforceVectorType() &&
         v1->getType() == v2->getType() &&
         mask->getType().enforceVectorType();
}

unique_ptr<Instr> FakeShuffle::dup(const string &suffix) const {
  return make_unique<FakeShuffle>(getType(), getName() + suffix,
                                            *v1, *v2, *mask);
}


vector<Value*> X86IntrinBinOp::operands() const {
  return { a, b };
}

void X86IntrinBinOp::rauw(const Value &what, Value &with) {
  RAUW(a);
  RAUW(b);
}

string X86IntrinBinOp::getOpName(Op op) {
  switch (op) {
#define PROCESS(NAME,A,B,C,D,E,F) case NAME: return #NAME;
#include "intrinsics.h"
#undef PROCESS
  }
  UNREACHABLE();
}

void X86IntrinBinOp::print(ostream &os) const {
  os << getName() << " = " << getOpName(op) << " " << *a << ", " << *b;
}

StateValue X86IntrinBinOp::toSMT(State &s) const {
  auto rty =    getType().getAsAggregateType();
  auto aty = a->getType().getAsAggregateType();
  auto bty = b->getType().getAsAggregateType();
  auto &av = s[*a];
  auto &bv = s[*b];

  switch (op) {
  // shift by one variable
  case x86_sse2_psrl_w:
  case x86_sse2_psrl_d:
  case x86_sse2_psrl_q:
  case x86_avx2_psrl_w:
  case x86_avx2_psrl_d:
  case x86_avx2_psrl_q:
  case x86_avx512_psrl_w_512:
  case x86_avx512_psrl_d_512:
  case x86_avx512_psrl_q_512:
  case x86_sse2_psra_w:
  case x86_sse2_psra_d:
  case x86_avx2_psra_w:
  case x86_avx2_psra_d:
  case x86_avx512_psra_q_128:
  case x86_avx512_psra_q_256:
  case x86_avx512_psra_w_512:
  case x86_avx512_psra_d_512:
  case x86_avx512_psra_q_512:
  case x86_sse2_psll_w:
  case x86_sse2_psll_d:
  case x86_sse2_psll_q:
  case x86_avx2_psll_w:
  case x86_avx2_psll_d:
  case x86_avx2_psll_q:
  case x86_avx512_psll_w_512:
  case x86_avx512_psll_d_512:
  case x86_avx512_psll_q_512:
  {
    vector<StateValue> vals;
    unsigned elem_bw = bty->getChild(0).bits();

    expr shift_np = true;
    expr shift_v;
    // extract lower 64 bits from b
    for (unsigned i = 0, e = 64 / elem_bw; i != e ; ++i) {
      StateValue vv = bty->extract(bv, i);
      shift_v = (i == 0) ? vv.value : vv.value.concat(shift_v);
      // if any elements in lower 64 bits is poison, the result is poison
      shift_np &= vv.non_poison;
    }
    function<expr(const expr&, const expr&)> fn;
    switch(op) {
    case x86_sse2_psrl_w:
    case x86_sse2_psrl_d:
    case x86_sse2_psrl_q:
    case x86_avx2_psrl_w:
    case x86_avx2_psrl_d:
    case x86_avx2_psrl_q:
    case x86_avx512_psrl_w_512:
    case x86_avx512_psrl_d_512:
    case x86_avx512_psrl_q_512:
      fn = [&](auto a, auto b) -> expr {
        return expr::mkIf(shift_v.uge(expr::mkUInt(elem_bw, 64)),
                          expr::mkUInt(0, elem_bw),
                          a.lshr(b));
      };
      break;
    case x86_sse2_psra_w:
    case x86_sse2_psra_d:
    case x86_avx2_psra_w:
    case x86_avx2_psra_d:
    case x86_avx512_psra_q_128:
    case x86_avx512_psra_q_256:
    case x86_avx512_psra_w_512:
    case x86_avx512_psra_d_512:
    case x86_avx512_psra_q_512:
      fn = [&](auto a, auto b) -> expr {
        return expr::mkIf(shift_v.uge(expr::mkUInt(elem_bw, 64)),
                          expr::mkIf(a.isNegative(),
                                     expr::mkUInt(-1, elem_bw),
                                     expr::mkUInt( 0, elem_bw)),
                          a.ashr(b));
      };
      break;
    case x86_sse2_psll_w:
    case x86_sse2_psll_d:
    case x86_sse2_psll_q:
    case x86_avx2_psll_w:
    case x86_avx2_psll_d:
    case x86_avx2_psll_q:
    case x86_avx512_psll_w_512:
    case x86_avx512_psll_d_512:
    case x86_avx512_psll_q_512:
      fn = [&](auto a, auto b) -> expr {
        return expr::mkIf(shift_v.uge(expr::mkUInt(elem_bw, 64)),
                          expr::mkUInt(0, elem_bw),
                          a << b);
      };
      break;
    default: UNREACHABLE();
    }
    for (unsigned i = 0, e = aty->numElementsConst(); i != e; ++i) {
      auto ai = aty->extract(av, i);
      expr shift = fn(ai.value, shift_v.trunc(elem_bw));
      vals.emplace_back(move(shift), shift_np && ai.non_poison);
    }
    return rty->aggregateVals(vals);
  }
  // vertical
  case x86_sse2_pavg_w:
  case x86_sse2_pavg_b:
  case x86_avx2_pavg_w:
  case x86_avx2_pavg_b:
  case x86_avx512_pavg_w_512:
  case x86_avx512_pavg_b_512:
  case x86_ssse3_psign_b_128:
  case x86_ssse3_psign_w_128:
  case x86_ssse3_psign_d_128:
  case x86_avx2_psign_b:
  case x86_avx2_psign_w:
  case x86_avx2_psign_d:
  case x86_avx2_psrlv_d:
  case x86_avx2_psrlv_d_256:
  case x86_avx2_psrlv_q:
  case x86_avx2_psrlv_q_256:
  case x86_avx512_psrlv_d_512:
  case x86_avx512_psrlv_q_512:
  case x86_avx512_psrlv_w_128:
  case x86_avx512_psrlv_w_256:
  case x86_avx512_psrlv_w_512:
  case x86_avx2_psrav_d:
  case x86_avx2_psrav_d_256:
  case x86_avx512_psrav_d_512:
  case x86_avx512_psrav_q_128:
  case x86_avx512_psrav_q_256:
  case x86_avx512_psrav_q_512:
  case x86_avx512_psrav_w_128:
  case x86_avx512_psrav_w_256:
  case x86_avx512_psrav_w_512:
  case x86_avx2_psllv_d:
  case x86_avx2_psllv_d_256:
  case x86_avx2_psllv_q:
  case x86_avx2_psllv_q_256:
  case x86_avx512_psllv_d_512:
  case x86_avx512_psllv_q_512:
  case x86_avx512_psllv_w_128:
  case x86_avx512_psllv_w_256:
  case x86_avx512_psllv_w_512:
  case x86_sse2_pmulh_w:
  case x86_avx2_pmulh_w:
  case x86_avx512_pmulh_w_512:
  case x86_sse2_pmulhu_w:
  case x86_avx2_pmulhu_w:
  case x86_avx512_pmulhu_w_512:
  {
    vector<StateValue> vals;
    function<expr(const expr&, const expr&)> fn;
    switch (op) {
    case x86_sse2_pavg_w:
    case x86_sse2_pavg_b:
    case x86_avx2_pavg_w:
    case x86_avx2_pavg_b:
    case x86_avx512_pavg_w_512:
    case x86_avx512_pavg_b_512:
      fn = [&](auto a, auto b) -> expr {
        unsigned bw = a.bits();
        return (a.zext(1) + b.zext(1) + expr::mkUInt(1, bw + 1)).lshr(expr::mkUInt(1, bw + 1)).trunc(bw);
      };
      break;
    case x86_ssse3_psign_b_128:
    case x86_ssse3_psign_w_128:
    case x86_ssse3_psign_d_128:
    case x86_avx2_psign_b:
    case x86_avx2_psign_w:
    case x86_avx2_psign_d:
      fn = [&](auto a, auto b) -> expr {
        return expr::mkIf(b.isZero(), b,
                          expr::mkIf(b.isNegative(),
                                     expr::mkUInt(0, a.bits()) - a,
                                     a));
      };
      break;
    case x86_avx2_psrlv_d:
    case x86_avx2_psrlv_d_256:
    case x86_avx2_psrlv_q:
    case x86_avx2_psrlv_q_256:
    case x86_avx512_psrlv_d_512:
    case x86_avx512_psrlv_q_512:
    case x86_avx512_psrlv_w_128:
    case x86_avx512_psrlv_w_256:
    case x86_avx512_psrlv_w_512:
      fn = [&](auto a, auto b) -> expr {
        unsigned bw = a.bits();
        return expr::mkIf(b.uge(expr::mkUInt(bw, bw)),
                          expr::mkUInt(0, bw),
                          a.lshr(b));
      };
      break;
    case x86_avx2_psrav_d:
    case x86_avx2_psrav_d_256:
    case x86_avx512_psrav_d_512:
    case x86_avx512_psrav_q_128:
    case x86_avx512_psrav_q_256:
    case x86_avx512_psrav_q_512:
    case x86_avx512_psrav_w_128:
    case x86_avx512_psrav_w_256:
    case x86_avx512_psrav_w_512:
      fn = [&](auto a, auto b) -> expr {
        unsigned bw = a.bits();
        return expr::mkIf(b.uge(expr::mkUInt(bw, bw)),
                          expr::mkIf(a.isNegative(),
                                     expr::mkUInt(-1, bw),
                                     expr::mkUInt( 0, bw)),
                          a.ashr(b));
      };
      break;
    case x86_avx2_psllv_d:
    case x86_avx2_psllv_d_256:
    case x86_avx2_psllv_q:
    case x86_avx2_psllv_q_256:
    case x86_avx512_psllv_d_512:
    case x86_avx512_psllv_q_512:
    case x86_avx512_psllv_w_128:
    case x86_avx512_psllv_w_256:
    case x86_avx512_psllv_w_512:
      fn = [&](auto a, auto b) -> expr {
        unsigned bw = a.bits();
        return expr::mkIf(b.uge(expr::mkUInt(bw, bw)),
                          expr::mkUInt(0, bw),
                          a << b);
      };
      break;
    case x86_sse2_pmulh_w:
    case x86_avx2_pmulh_w:
    case x86_avx512_pmulh_w_512:
      fn = [&](auto a, auto b) -> expr {
        expr mul = a.sext(16) * b.sext(16);
        return mul.extract(31, 16);
      };
      break;
    case x86_sse2_pmulhu_w:
    case x86_avx2_pmulhu_w:
    case x86_avx512_pmulhu_w_512:
      fn = [&](auto a, auto b) -> expr {
        expr mul = a.zext(16) * b.zext(16);
        return mul.extract(31, 16);
      };
      break;
    default: UNREACHABLE();
    }
    for (unsigned i = 0, e = rty->numElementsConst(); i != e; ++i) {
      auto ai = aty->extract(av, i);
      auto bi = bty->extract(bv, i);
      vals.emplace_back(fn(ai.value, bi.value),
                        ai.non_poison && bi.non_poison);
    }
    return rty->aggregateVals(vals);
  }
  // pshuf.b
  case x86_ssse3_pshuf_b_128:
  case x86_avx2_pshuf_b:
  case x86_avx512_pshuf_b_512:
  {
    auto avty = static_cast<const VectorType*>(aty);
    vector<StateValue> vals;
    unsigned laneCount = shape_ret[op].first;
    for (unsigned i = 0; i != laneCount; ++i) {
      auto [b, bp] = bty->extract(bv, i);
      expr id = (b & expr::mkUInt(0x0F, 8)) + (expr::mkUInt(i & 0x30, 8));
      auto [r, rp] = avty->extract(av, id);
      auto ai = expr::mkIf(b.extract(7, 7) == expr::mkUInt(0, 1), r,
                           expr::mkUInt(0, 8));

      vals.emplace_back(move(ai), bp && rp);
    }
    return rty->aggregateVals(vals);
  }
  /*
  case mmx_punpckhbw:
  case mmx_punpckhwd:
  case mmx_punpckhdq:
  case mmx_punpcklbw:
  case mmx_punpcklwd:
  case mmx_punpckldq:
  {
    vector<StateValue> vals;
    unsigned laneCount, startVal, endVal;
    switch (op) {
    case mmx_punpckhbw: laneCount = 8; startVal = laneCount / 2; endVal = laneCount; break;
    case mmx_punpckhwd: laneCount = 4; startVal = laneCount / 2; endVal = laneCount; break;
    case mmx_punpckhdq: laneCount = 2; startVal = laneCount / 2; endVal = laneCount; break;
    case mmx_punpcklbw: laneCount = 8; startVal = 0; endVal = laneCount / 2; break;
    case mmx_punpcklwd: laneCount = 4; startVal = 0; endVal = laneCount / 2; break;
    case mmx_punpckldq: laneCount = 2; startVal = 0; endVal = laneCount / 2; break;
    default: UNREACHABLE();
    }
    //Starts at first lane of high half of both vectors
    for (unsigned i = startVal; i != endVal; ++i) {
      auto ai = aty->extract(av, i);
      auto bi = bty->extract(bv, i);

      vals.emplace_back(move(ai));
      vals.emplace_back(move(bi));
    }

    return rty->aggregateVals(vals);
  }*/
  // horizontal
  case x86_ssse3_phadd_w_128:
  case x86_ssse3_phadd_d_128:
  case x86_ssse3_phadd_sw_128:
  case x86_avx2_phadd_w:
  case x86_avx2_phadd_d:
  case x86_avx2_phadd_sw:
  case x86_ssse3_phsub_w_128:
  case x86_ssse3_phsub_d_128:
  case x86_ssse3_phsub_sw_128:
  case x86_avx2_phsub_w:
  case x86_avx2_phsub_d:
  case x86_avx2_phsub_sw: {
    vector<StateValue> vals;
    unsigned laneCount = shape_ret[op].first;
    unsigned groupsize = 128/shape_ret[op].second;
    function<expr(const expr&, const expr&)> fn;
    switch (op) {
    case x86_ssse3_phadd_w_128:
    case x86_ssse3_phadd_d_128:
    case x86_avx2_phadd_w:
    case x86_avx2_phadd_d:
      fn = [&](auto a, auto b) -> expr {
        return a + b;
      };
      break;
    case x86_ssse3_phadd_sw_128:
    case x86_avx2_phadd_sw:
      fn = [&](auto a, auto b) -> expr {
        return a.sadd_sat(b);
      };
      break;
    case x86_ssse3_phsub_w_128:
    case x86_ssse3_phsub_d_128:
    case x86_avx2_phsub_w:
    case x86_avx2_phsub_d:
      fn = [&](auto a, auto b) -> expr {
        return a - b;
      };
      break;
    case x86_ssse3_phsub_sw_128:
    case x86_avx2_phsub_sw:
      fn = [&](auto a, auto b) -> expr {
        return a.ssub_sat(b);
      };
      break;
    default: UNREACHABLE();
    }
    for (unsigned j = 0; j != laneCount / groupsize; j ++) {
      for (unsigned i = 0; i != groupsize; i += 2) {
        auto [a1, p1] = aty->extract(av, j * groupsize + i);
        auto [a2, p2] = aty->extract(av, j * groupsize + i + 1);
        vals.emplace_back(fn(a1, a2), p1 && p2);
      }
      for (unsigned i = 0; i != groupsize; i += 2) {
        auto [b1, p1] = aty->extract(bv, j * groupsize + i);
        auto [b2, p2] = aty->extract(bv, j * groupsize + i + 1);
        vals.emplace_back(fn(b1, b2), p1 && p2);
      }
    }
    return rty->aggregateVals(vals);
  }
  case x86_sse2_psrli_w:
  case x86_sse2_psrli_d:
  case x86_sse2_psrli_q:
  case x86_avx2_psrli_w:
  case x86_avx2_psrli_d:
  case x86_avx2_psrli_q:
  case x86_avx512_psrli_w_512:
  case x86_avx512_psrli_d_512:
  case x86_avx512_psrli_q_512:
  case x86_sse2_psrai_w:
  case x86_sse2_psrai_d:
  case x86_avx2_psrai_w:
  case x86_avx2_psrai_d:
  case x86_avx512_psrai_w_512:
  case x86_avx512_psrai_d_512:
  case x86_avx512_psrai_q_128:
  case x86_avx512_psrai_q_256:
  case x86_avx512_psrai_q_512:
  case x86_sse2_pslli_w:
  case x86_sse2_pslli_d:
  case x86_sse2_pslli_q:
  case x86_avx2_pslli_w:
  case x86_avx2_pslli_d:
  case x86_avx2_pslli_q:
  case x86_avx512_pslli_w_512:
  case x86_avx512_pslli_d_512:
  case x86_avx512_pslli_q_512: {
    vector<StateValue> vals;
    function<expr(const expr&, const expr&)> fn;
    switch (op) {
    case x86_sse2_psrai_w:
    case x86_sse2_psrai_d:
    case x86_avx2_psrai_w:
    case x86_avx2_psrai_d:
    case x86_avx512_psrai_w_512:
    case x86_avx512_psrai_d_512:
    case x86_avx512_psrai_q_128:
    case x86_avx512_psrai_q_256:
    case x86_avx512_psrai_q_512:
      fn = [&](auto a, auto b) -> expr {
        unsigned sz_a = a.bits();
        expr check = b.uge(expr::mkUInt(sz_a, 32));
        expr outbounds = expr::mkIf(a.isNegative(),
                                    expr::mkInt(-1, sz_a),
                                    expr::mkUInt(0, sz_a));
        expr inbounds = a.ashr(b.zextOrTrunc(sz_a));
        return expr::mkIf(move(check), move(outbounds), move(inbounds));
      };
      break;
    case x86_sse2_psrli_w:
    case x86_sse2_psrli_d:
    case x86_sse2_psrli_q:
    case x86_avx2_psrli_w:
    case x86_avx2_psrli_d:
    case x86_avx2_psrli_q:
    case x86_avx512_psrli_w_512:
    case x86_avx512_psrli_d_512:
    case x86_avx512_psrli_q_512:
      fn = [&](auto a, auto b) -> expr {
        unsigned sz_a = a.bits();
        expr check = b.uge(expr::mkUInt(sz_a, 32));
        expr outbounds = expr::mkUInt(0, sz_a);
        expr inbounds = a.lshr(b.zextOrTrunc(sz_a));
        return expr::mkIf(move(check), move(outbounds), move(inbounds));
      };
      break;
    case x86_sse2_pslli_w:
    case x86_sse2_pslli_d:
    case x86_sse2_pslli_q:
    case x86_avx2_pslli_w:
    case x86_avx2_pslli_d:
    case x86_avx2_pslli_q:
    case x86_avx512_pslli_w_512:
    case x86_avx512_pslli_d_512:
    case x86_avx512_pslli_q_512:
      fn = [&](auto a, auto b) -> expr {
        unsigned sz_a = a.bits();
        expr check = b.uge(expr::mkUInt(sz_a, 32));
        expr outbounds = expr::mkUInt(0, sz_a);
        expr inbounds = a << b.zextOrTrunc(sz_a);
        return expr::mkIf(move(check), move(outbounds), move(inbounds));
      };
      break;
    default: UNREACHABLE();
    }
    for (unsigned i = 0, e = rty->numElementsConst(); i != e; ++i) {
      auto ai = aty->extract(av, i);
      vals.emplace_back(fn(ai.value, bv.value),
                        ai.non_poison && bv.non_poison);
    }
    return rty->aggregateVals(vals);
  }
  case x86_sse2_pmadd_wd:
  case x86_avx2_pmadd_wd:
  case x86_avx512_pmaddw_d_512:
  case x86_ssse3_pmadd_ub_sw_128:
  case x86_avx2_pmadd_ub_sw:
  case x86_avx512_pmaddubs_w_512: {
    vector<StateValue> vals;
    for (unsigned i = 0, e = shape_ret[op].first; i != e; ++i) {
      auto [a1, a1p] = aty->extract(av, i * 2);
      auto [a2, a2p] = aty->extract(av, i * 2 + 1);
      auto [b1, b1p] = bty->extract(bv, i * 2);
      auto [b2, b2p] = bty->extract(bv, i * 2 + 1);

      auto np = a1p && a2p && b1p && b2p;

      if (op == x86_sse2_pmadd_wd ||
          op == x86_avx2_pmadd_wd ||
          op == x86_avx512_pmaddw_d_512) {
        expr v = a1.sext(16) * b1.sext(16) + a2.sext(16) * b2.sext(16);
        vals.emplace_back(move(v), move(np));
      } else {
        expr v = (a1.zext(8) * b1.sext(8)).sadd_sat(a2.zext(8) * b2.sext(8));
        vals.emplace_back(move(v), move(np));
      }
    }
    return rty->aggregateVals(vals);
  }
  case x86_sse2_packsswb_128:
  case x86_avx2_packsswb:
  case x86_avx512_packsswb_512:
  case x86_sse2_packuswb_128:
  case x86_avx2_packuswb:
  case x86_avx512_packuswb_512:
  case x86_sse2_packssdw_128:
  case x86_avx2_packssdw:
  case x86_avx512_packssdw_512:
  case x86_sse41_packusdw:
  case x86_avx2_packusdw:
  case x86_avx512_packusdw_512: {
    vector<StateValue> vals;
    function<expr(const expr&)> fn;
    if (op == x86_sse2_packsswb_128 || op == x86_avx2_packsswb ||
        op == x86_avx512_packsswb_512 || op == x86_sse2_packssdw_128 ||
        op == x86_avx2_packssdw || op == x86_avx512_packssdw_512) {
      fn = [&](auto a) -> expr {
        unsigned bw = a.bits() / 2;
        auto min = expr::IntSMin(bw);
        auto max = expr::IntSMax(bw);
        return expr::mkIf(a.sle(min.sext(bw)), min,
                          expr::mkIf(a.sge(max.sext(bw)), max,
                                     a.trunc(bw)));
      };
    } else {
      fn = [&](auto a) -> expr {
        unsigned bw = a.bits() / 2;
        auto max = expr::IntUMax(bw);
        return expr::mkIf(a.uge(max.zext(bw)), max, a.trunc(bw));
      };
    }

    unsigned groupsize = 128/shape_op1[op].second;
    unsigned laneCount = shape_op1[op].first;
    for (unsigned j = 0; j != laneCount / groupsize; j ++) {
      for (unsigned i = 0; i != groupsize; i ++) {
        auto [a1, p1] = aty->extract(av, j * groupsize + i);
        vals.emplace_back(fn(move(a1)), move(p1));
      }
      for (unsigned i = 0; i != groupsize; i ++) {
        auto [b1, p1] = aty->extract(bv, j * groupsize + i);
        vals.emplace_back(fn(move(b1)), move(p1));
      }
    }
    return rty->aggregateVals(vals);
  }
  // TODO: add semantic for other intrinsics
  default:
    UNREACHABLE();
  }
}

expr X86IntrinBinOp::getTypeConstraints(const Function &f) const {
  return Value::getTypeConstraints() &&
    (shape_op0[op].first != 1
      ? a->getType().enforceVectorType(
          [this](auto &ty) {return ty.enforceIntType(shape_op0[op].second);}) &&
        a->getType().getAsAggregateType()->numElements() == shape_op0[op].first
      : a->getType().enforceIntType(shape_op0[op].second)) &&
    (shape_op1[op].first != 1
      ? b->getType().enforceVectorType(
          [this](auto &ty) {return ty.enforceIntType(shape_op1[op].second);}) &&
        b->getType().getAsAggregateType()->numElements() == shape_op1[op].first
      : b->getType().enforceIntType(shape_op1[op].second)) &&
    (shape_ret[op].first != 1
      ? getType().enforceVectorType(
          [this](auto &ty) {return ty.enforceIntType(shape_ret[op].second);}) &&
        getType().getAsAggregateType()->numElements() == shape_ret[op].first
      : getType().enforceIntType(shape_ret[op].second));
}

unique_ptr<Instr> X86IntrinBinOp::dup(const string &suffix) const {
  return make_unique<X86IntrinBinOp>(getType(), getName() + suffix, *a, *b, op);
}


const ConversionOp* isCast(ConversionOp::Op op, const Value &v) {
  auto c = dynamic_cast<const ConversionOp*>(&v);
  return (c && c->getOp() == op) ? c : nullptr;
}

bool hasNoSideEffects(const Instr &i) {
  return isNoOp(i) ||
         dynamic_cast<const ConversionOp*>(&i) ||
         dynamic_cast<const ExtractValue*>(&i) ||
         dynamic_cast<const Freeze*>(&i) ||
         dynamic_cast<const GEP*>(&i) ||
         dynamic_cast<const ICmp*>(&i) ||
         dynamic_cast<const InsertValue*>(&i) ||
         dynamic_cast<const ShuffleVector*>(&i);
}

Value* isNoOp(const Value &v) {
  if (auto *c = isCast(ConversionOp::BitCast, v))
    return &c->getValue();

  if (auto gep = dynamic_cast<const GEP*>(&v))
    return gep->getMaxGEPOffset() == 0 ? &gep->getPtr() : nullptr;

  if (auto unop = dynamic_cast<const UnaryOp*>(&v)) {
    if (unop->getOp() == UnaryOp::Copy)
      return &unop->getValue();
  }

  return nullptr;
}
}
