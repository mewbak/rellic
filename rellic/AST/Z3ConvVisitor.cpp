/*
 * Copyright (c) 2018 Trail of Bits, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define GOOGLE_STRIP_LOG 1

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "rellic/AST/Util.h"
#include "rellic/AST/Z3ConvVisitor.h"

namespace rellic {

namespace {

static unsigned GetZ3SortSize(z3::sort sort) {
  switch (sort.sort_kind()) {
    case Z3_BOOL_SORT:
      return 1;
      break;

    case Z3_BV_SORT:
      return sort.bv_size();
      break;

    case Z3_FLOATING_POINT_SORT: {
      auto &ctx = sort.ctx();
      return Z3_fpa_get_sbits(ctx, sort) + Z3_fpa_get_ebits(ctx, sort);
    } break;

    case Z3_UNINTERPRETED_SORT:
      return 0;
      break;

    default:
      LOG(FATAL) << "Unknown Z3 sort: " << sort;
      break;
  }
}

static std::string CreateZ3DeclName(clang::NamedDecl *decl) {
  std::stringstream ss;
  ss << std::hex << decl << std::dec;
  ss << '_' << decl->getNameAsString();
  return ss.str();
}

static z3::expr CreateZ3BitwiseCast(z3::expr expr, size_t src, size_t dst,
                                    bool sign) {
  CHECK(expr.is_bv()) << "z3::expr is not a bitvector!";
  int64_t diff = dst - src;
  // extend
  if (diff > 0) {
    return sign ? z3::sext(expr, diff) : z3::zext(expr, diff);
  }
  // truncate
  if (diff < 0) {
    return expr.extract(dst, 1);
  }
  // nothing
  return expr;
}

}  // namespace

Z3ConvVisitor::Z3ConvVisitor(clang::ASTContext *c_ctx, z3::context *z3_ctx)
    : ast_ctx(c_ctx),
      z3_ctx(z3_ctx),
      z3_expr_vec(*z3_ctx),
      z3_decl_vec(*z3_ctx) {}

// Inserts a `clang::Expr` <=> `z3::expr` mapping into
void Z3ConvVisitor::InsertZ3Expr(clang::Expr *c_expr, z3::expr z_expr) {
  auto iter = z3_expr_map.find(c_expr);
  CHECK(iter == z3_expr_map.end());
  z3_expr_map[c_expr] = z3_expr_vec.size();
  z3_expr_vec.push_back(z_expr);
}

// Retrieves a `z3::expr` corresponding to `c_expr`.
// The `z3::expr` needs to be created and inserted by
// `Z3ConvVisistor::InsertZ3Expr` first.
z3::expr Z3ConvVisitor::GetZ3Expr(clang::Expr *c_expr) {
  auto iter = z3_expr_map.find(c_expr);
  CHECK(iter != z3_expr_map.end());
  return z3_expr_vec[iter->second];
}

// Inserts a `clang::ValueDecl` <=> `z3::func_decl` mapping into
void Z3ConvVisitor::InsertZ3Decl(clang::ValueDecl *c_decl,
                                 z3::func_decl z_decl) {
  auto iter = z3_decl_map.find(c_decl);
  CHECK(iter == z3_decl_map.end());
  z3_decl_map[c_decl] = z3_decl_vec.size();
  z3_decl_vec.push_back(z_decl);
}

// Retrieves a `z3::func_decl` corresponding to `c_decl`.
// The `z3::func_decl` needs to be created and inserted by
// `Z3ConvVisistor::InsertZ3Decl` first.
z3::func_decl Z3ConvVisitor::GetZ3Decl(clang::ValueDecl *c_decl) {
  auto iter = z3_decl_map.find(c_decl);
  CHECK(iter != z3_decl_map.end());
  return z3_decl_vec[iter->second];
}

// If `expr` is not boolean, returns a `z3::expr` that corresponds
// to the non-boolean to boolean expression cast in C. Otherwise
// returns `expr`.
z3::expr Z3ConvVisitor::Z3BoolCast(z3::expr expr) {
  if (expr.is_bool()) {
    return expr;
  } else {
    auto cast = expr != z3_ctx->num_val(0, expr.get_sort());
    return cast.simplify();
  }
}

void Z3ConvVisitor::InsertCExpr(z3::expr z_expr, clang::Expr *c_expr) {
  auto hash = z_expr.hash();
  auto iter = c_expr_map.find(hash);
  CHECK(iter == c_expr_map.end());
  c_expr_map[hash] = c_expr;
}

clang::Expr *Z3ConvVisitor::GetCExpr(z3::expr z_expr) {
  auto hash = z_expr.hash();
  auto iter = c_expr_map.find(hash);
  CHECK(iter != c_expr_map.end());
  return c_expr_map[hash];
}

void Z3ConvVisitor::InsertCValDecl(z3::func_decl z_decl,
                                   clang::ValueDecl *c_decl) {
  auto id = Z3_get_func_decl_id(*z3_ctx, z_decl);
  auto iter = c_decl_map.find(id);
  CHECK(iter == c_decl_map.end());
  c_decl_map[id] = c_decl;
}

clang::ValueDecl *Z3ConvVisitor::GetCValDecl(z3::func_decl z_decl) {
  auto id = Z3_get_func_decl_id(*z3_ctx, z_decl);
  auto iter = c_decl_map.find(id);
  CHECK(iter != c_decl_map.end());
  return c_decl_map[id];
}

z3::sort Z3ConvVisitor::GetZ3Sort(clang::QualType type) {
  // Booleans
  if (type->isBooleanType()) {
    return z3_ctx->bool_sort();
  }
  // Structures
  if (type->isStructureType()) {
    auto decl = clang::cast<clang::RecordType>(type)->getDecl();
    auto name = decl->getNameAsString().c_str();
    return z3_ctx->uninterpreted_sort(name);
  }
  auto bitwidth = ast_ctx->getTypeSize(type);
  // Floating points
  if (type->isRealFloatingType()) {
    switch (bitwidth) {
      case 16:
        // return z3_ctx.fpa_sort<16>();
        return z3::to_sort(*z3_ctx, Z3_mk_fpa_sort_16(*z3_ctx));
        break;

      case 32:
        return z3::to_sort(*z3_ctx, Z3_mk_fpa_sort_32(*z3_ctx));
        break;

      case 64:
        return z3::to_sort(*z3_ctx, Z3_mk_fpa_sort_64(*z3_ctx));
        break;

      case 128:
        return z3::to_sort(*z3_ctx, Z3_mk_fpa_sort_128(*z3_ctx));
        break;

      default:
        LOG(FATAL) << "Unsupported floating-point bitwidth!";
        break;
    }
  }
  // Default to bitvectors
  return z3::to_sort(*z3_ctx, Z3_mk_bv_sort(*z3_ctx, bitwidth));
}

clang::Expr *Z3ConvVisitor::CreateLiteralExpr(z3::expr z_expr) {
  DLOG(INFO) << "Creating literal clang::Expr for " << z_expr;

  auto sort = z_expr.get_sort();

  clang::Expr *result = nullptr;

  switch (sort.sort_kind()) {
    case Z3_BOOL_SORT: {
      auto type = ast_ctx->UnsignedIntTy;
      auto size = ast_ctx->getIntWidth(type);
      llvm::APInt val(size, z_expr.bool_value() == Z3_L_TRUE ? 1 : 0);
      result = CreateIntegerLiteral(*ast_ctx, val, type);
    } break;

    case Z3_BV_SORT: {
      auto type = ast_ctx->getIntTypeForBitwidth(GetZ3SortSize(sort), 0);
      auto size = ast_ctx->getIntWidth(type);
      llvm::APInt val(size, Z3_get_numeral_string(z_expr.ctx(), z_expr), 10);
      if (type->isCharType()) {
        result = CreateCharacterLiteral(*ast_ctx, val, type);
      } else {
        result = CreateIntegerLiteral(*ast_ctx, val, type);
      }
    } break;

    case Z3_FLOATING_POINT_SORT: {
      auto type = ast_ctx->getRealTypeForBitwidth(GetZ3SortSize(sort));
      auto size = ast_ctx->getTypeSize(type);
      const llvm::fltSemantics *semantics;
      switch (size) {
        case 16:
          semantics = &llvm::APFloat::IEEEhalf();
          break;
        case 32:
          semantics = &llvm::APFloat::IEEEsingle();
          break;
        case 64:
          semantics = &llvm::APFloat::IEEEdouble();
          break;
        case 128:
          semantics = &llvm::APFloat::IEEEquad();
          break;
        default:
          LOG(FATAL) << "Unknown Z3 floating-point sort!";
          break;
      }
      llvm::APInt ival(size, Z3_get_numeral_string(z_expr.ctx(), z_expr), 10);
      llvm::APFloat fval(*semantics, ival);
      result = CreateFloatingLiteral(*ast_ctx, fval, type);
    } break;

    default:
      LOG(FATAL) << "Unknown Z3 sort: " << sort;
      break;
  }

  return result;
}

// Retrieves or creates`z3::expr`s from `clang::Expr`.
z3::expr Z3ConvVisitor::GetOrCreateZ3Expr(clang::Expr *c_expr) {
  if (!z3_expr_map.count(c_expr)) {
    TraverseStmt(c_expr);
  }
  return GetZ3Expr(c_expr);
}

z3::func_decl Z3ConvVisitor::GetOrCreateZ3Decl(clang::ValueDecl *c_decl) {
  if (!z3_decl_map.count(c_decl)) {
    TraverseDecl(c_decl);
  }

  auto z_decl = GetZ3Decl(c_decl);

  auto id = Z3_get_func_decl_id(*z3_ctx, z_decl);
  if (!c_decl_map.count(id)) {
    InsertCValDecl(z_decl, c_decl);
  }

  return z_decl;
}

// Retrieves or creates `clang::Expr` from `z3::expr`.
clang::Expr *Z3ConvVisitor::GetOrCreateCExpr(z3::expr z_expr) {
  if (!c_expr_map.count(z_expr.hash())) {
    VisitZ3Expr(z_expr);
  }
  return GetCExpr(z_expr);
}

bool Z3ConvVisitor::VisitVarDecl(clang::VarDecl *var) {
  auto name = var->getNameAsString().c_str();
  DLOG(INFO) << "VisitVarDecl: " << name;
  if (z3_decl_map.count(var)) {
    DLOG(INFO) << "Re-declaration of " << name << "; Returning.";
    return true;
  }

  auto z_name = CreateZ3DeclName(var);
  auto z_sort = GetZ3Sort(var->getType());
  auto z_const = z3_ctx->constant(z_name.c_str(), z_sort);

  InsertZ3Decl(var, z_const.decl());

  return true;
}

bool Z3ConvVisitor::VisitFieldDecl(clang::FieldDecl *field) {
  auto name = field->getNameAsString().c_str();
  DLOG(INFO) << "VisitFieldDecl: " << name;
  if (z3_decl_map.count(field)) {
    DLOG(INFO) << "Re-declaration of " << name << "; Returning.";
    return true;
  }

  auto z_name = CreateZ3DeclName(field->getParent()) + "_" + name;
  auto z_sort = GetZ3Sort(field->getType());
  auto z_const = z3_ctx->constant(z_name.c_str(), z_sort);

  InsertZ3Decl(field, z_const.decl());

  return true;
}

bool Z3ConvVisitor::VisitFunctionDecl(clang::FunctionDecl *func) {
  DLOG(INFO) << "VisitFunctionDecl";
  LOG(FATAL) << "Unimplemented FunctionDecl visitor";
  return true;
}

bool Z3ConvVisitor::VisitCStyleCastExpr(clang::CStyleCastExpr *c_cast) {
  DLOG(INFO) << "VisitCStyleCastExpr";
  if (z3_expr_map.count(c_cast)) {
    return true;
  }

  // C exprs
  auto c_sub = c_cast->getSubExpr();
  // C types
  auto t_src = c_sub->getType();
  auto t_dst = c_cast->getType();
  // C type sizes
  auto t_src_size = ast_ctx->getTypeSize(t_src);
  auto t_dst_size = ast_ctx->getTypeSize(t_dst);
  // Z3 exprs
  auto z_sub = GetOrCreateZ3Expr(c_sub);

  auto z_cast = CreateZ3BitwiseCast(z_sub, t_src_size, t_dst_size,
                                    t_src->isSignedIntegerType());
  // Z3 cast function
  // auto ZCastFunc = [this, &z_sub, &z_cast](const char *name) {
  //   auto s_src = z_sub.get_sort();
  //   auto s_dst = z_cast.get_sort();
  //   auto z_func = z3_ctx->function(name, s_src, s_dst);
  //   return z_func(z_sub);
  // };

  switch (c_cast->getCastKind()) {
    case clang::CastKind::CK_PointerToIntegral: {
      auto s_src = z_sub.get_sort();
      auto s_dst = z_cast.get_sort();
      auto z_func = z3_ctx->function("PtrToInt", s_src, s_dst);
      z_cast = z_func(z_sub);
    } break;

    case clang::CastKind::CK_IntegralToPointer: {
      auto s_src = z_sub.get_sort();
      auto s_dst = z_cast.get_sort();
      auto t_dst_opaque_ptr_val =
          reinterpret_cast<uint64_t>(t_dst.getAsOpaquePtr());
      auto z_ptr = z3_ctx->bv_val(t_dst_opaque_ptr_val, 8 * sizeof(void *));
      auto s_ptr = z_ptr.get_sort();
      auto z_func = z3_ctx->function("IntToPtr", s_ptr, s_src, s_dst);
      z_cast = z_func(z_ptr, z_sub);
    } break;

    case clang::CastKind::CK_IntegralCast:
    case clang::CastKind::CK_NullToPointer:
      break;

    default:
      LOG(FATAL) << "Unsupported cast type: " << c_cast->getCastKindName();
      break;
  }

  // Save
  InsertZ3Expr(c_cast, z_cast);

  return true;
}

bool Z3ConvVisitor::VisitImplicitCastExpr(clang::ImplicitCastExpr *c_cast) {
  DLOG(INFO) << "VisitImplicitCastExpr";
  if (z3_expr_map.count(c_cast)) {
    return true;
  }

  auto c_sub = c_cast->getSubExpr();
  auto z_sub = GetOrCreateZ3Expr(c_sub);

  switch (c_cast->getCastKind()) {
    case clang::CastKind::CK_ArrayToPointerDecay: {
      CHECK(z_sub.is_bv()) << "Pointer cast operand is not a bit-vector";
      auto s_ptr = GetZ3Sort(c_cast->getType());
      auto s_arr = z_sub.get_sort();
      auto z_func = z3_ctx->function("PtrDecay", s_arr, s_ptr);
      InsertZ3Expr(c_cast, z_func(z_sub));
    } break;

    default:
      LOG(FATAL) << "Unsupported cast type: " << c_cast->getCastKindName();
      break;
  }

  return true;
}

bool Z3ConvVisitor::VisitArraySubscriptExpr(clang::ArraySubscriptExpr *sub) {
  DLOG(INFO) << "VisitArraySubscriptExpr";
  if (z3_expr_map.count(sub)) {
    return true;
  }
  // Get base
  auto z_base = GetOrCreateZ3Expr(sub->getBase());
  auto base_sort = z_base.get_sort();
  CHECK(base_sort.is_bv()) << "Invalid Z3 sort for base expression";
  // Get index
  auto z_idx = GetOrCreateZ3Expr(sub->getIdx());
  auto idx_sort = z_idx.get_sort();
  CHECK(idx_sort.is_bv()) << "Invalid Z3 sort for index expression";
  // Get result
  auto elm_sort = GetZ3Sort(sub->getType());
  // Create a z_function
  auto z_arr_sub = z3_ctx->function("ArraySub", base_sort, idx_sort, elm_sort);
  // Create a z3 expression
  InsertZ3Expr(sub, z_arr_sub(z_base, z_idx));
  // Done
  return true;
}

bool Z3ConvVisitor::VisitMemberExpr(clang::MemberExpr *expr) {
  DLOG(INFO) << "VisitMemberExpr";
  if (z3_expr_map.count(expr)) {
    return true;
  }

  auto z_mem = GetOrCreateZ3Decl(expr->getMemberDecl())();
  auto z_base = GetOrCreateZ3Expr(expr->getBase());
  auto z_mem_expr = z3_ctx->function("Member", z_base.get_sort(),
                                     z_mem.get_sort(), z_mem.get_sort());

  InsertZ3Expr(expr, z_mem_expr(z_base, z_mem));

  return true;
}

bool Z3ConvVisitor::VisitCallExpr(clang::CallExpr *call) {
  LOG(FATAL) << "Unimplemented CallExpr visitor";
  return true;
}

// Translates clang unary operators expressions to Z3 equivalents.
bool Z3ConvVisitor::VisitParenExpr(clang::ParenExpr *parens) {
  DLOG(INFO) << "VisitParenExpr";
  if (z3_expr_map.count(parens)) {
    return true;
  }

  auto z_sub = GetOrCreateZ3Expr(parens->getSubExpr());

  switch (z_sub.decl().decl_kind()) {
    // Parens may affect semantics of C expressions
    case Z3_OP_UNINTERPRETED: {
      auto sort = z_sub.get_sort();
      auto z_paren = z3_ctx->function("Paren", sort, sort);
      InsertZ3Expr(parens, z_paren(z_sub));
    } break;
    // Default to ignoring the parens, Z3 should know how
    // to interpret them.
    default:
      InsertZ3Expr(parens, z_sub);
      break;
  }

  return true;
}

// Translates clang unary operators expressions to Z3 equivalents.
bool Z3ConvVisitor::VisitUnaryOperator(clang::UnaryOperator *c_op) {
  DLOG(INFO) << "VisitUnaryOperator: "
             << c_op->getOpcodeStr(c_op->getOpcode()).str();
  if (z3_expr_map.count(c_op)) {
    return true;
  }
  // Get operand
  auto operand = GetOrCreateZ3Expr(c_op->getSubExpr());
  // Create z3 unary op
  switch (c_op->getOpcode()) {
    case clang::UO_LNot: {
      InsertZ3Expr(c_op, !Z3BoolCast(operand));
    } break;

    case clang::UO_AddrOf: {
      auto ptr_sort = GetZ3Sort(c_op->getType());
      auto z_addrof = z3_ctx->function("AddrOf", operand.get_sort(), ptr_sort);
      InsertZ3Expr(c_op, z_addrof(operand));
    } break;

    case clang::UO_Deref: {
      auto elm_sort = GetZ3Sort(c_op->getType());
      auto z_deref = z3_ctx->function("Deref", operand.get_sort(), elm_sort);
      InsertZ3Expr(c_op, z_deref(operand));
    } break;

    default:
      LOG(FATAL) << "Unknown clang::UnaryOperator operation!";
      break;
  }
  return true;
}

// Translates clang binary operators expressions to Z3 equivalents.
bool Z3ConvVisitor::VisitBinaryOperator(clang::BinaryOperator *c_op) {
  DLOG(INFO) << "VisitBinaryOperator: " << c_op->getOpcodeStr().str();
  if (z3_expr_map.count(c_op)) {
    return true;
  }
  // Get operands
  auto lhs = GetOrCreateZ3Expr(c_op->getLHS());
  auto rhs = GetOrCreateZ3Expr(c_op->getRHS());
  // Create z3 binary op
  switch (c_op->getOpcode()) {
    case clang::BO_LAnd:
      InsertZ3Expr(c_op, Z3BoolCast(lhs) && Z3BoolCast(rhs));
      break;

    case clang::BO_LOr:
      InsertZ3Expr(c_op, Z3BoolCast(lhs) || Z3BoolCast(rhs));
      break;

    case clang::BO_EQ:
      InsertZ3Expr(c_op, lhs == rhs);
      break;

    case clang::BO_NE:
      InsertZ3Expr(c_op, lhs != rhs);
      break;

    case clang::BO_Rem:
      InsertZ3Expr(c_op, z3::srem(lhs, rhs));
      break;

    case clang::BO_Add:
      InsertZ3Expr(c_op, lhs + rhs);
      break;

    case clang::BO_Sub:
      InsertZ3Expr(c_op, lhs - rhs);
      break;

    case clang::BO_And:
      InsertZ3Expr(c_op, lhs & rhs);
      break;

    case clang::BO_Xor:
      InsertZ3Expr(c_op, lhs ^ rhs);
      break;

    case clang::BO_Shr:
      InsertZ3Expr(c_op, c_op->getLHS()->getType()->isSignedIntegerType()
                             ? z3::ashr(lhs, rhs)
                             : z3::lshr(lhs, rhs));
      break;

    default:
      LOG(FATAL) << "Unknown clang::BinaryOperator operation!";
      break;
  }
  return true;
}

// Translates clang variable references to Z3 constants.
bool Z3ConvVisitor::VisitDeclRefExpr(clang::DeclRefExpr *c_ref) {
  auto ref_decl = c_ref->getDecl();
  auto ref_name = ref_decl->getNameAsString();
  DLOG(INFO) << "VisitDeclRefExpr: " << ref_name;
  if (z3_expr_map.count(c_ref)) {
    return true;
  }

  auto z_const = GetOrCreateZ3Decl(ref_decl);
  InsertZ3Expr(c_ref, z_const());

  return true;
}

// Translates clang character literals references to Z3 numeral values.
bool Z3ConvVisitor::VisitCharacterLiteral(clang::CharacterLiteral *c_lit) {
  auto c_val = c_lit->getValue();
  DLOG(INFO) << "VisitCharacterLiteral: " << c_val;
  if (z3_expr_map.count(c_lit)) {
    return true;
  }

  auto z_sort = GetZ3Sort(c_lit->getType());
  auto z_val = z3_ctx->num_val(c_val, z_sort);
  InsertZ3Expr(c_lit, z_val);

  return true;
}

// Translates clang integer literals references to Z3 numeral values.
bool Z3ConvVisitor::VisitIntegerLiteral(clang::IntegerLiteral *c_lit) {
  auto c_val = c_lit->getValue().getLimitedValue();
  DLOG(INFO) << "VisitIntegerLiteral: " << c_val;
  if (z3_expr_map.count(c_lit)) {
    return true;
  }

  auto z_sort = GetZ3Sort(c_lit->getType());
  auto z_val = z_sort.is_bool() ? z3_ctx->bool_val(c_val != 0)
                                : z3_ctx->num_val(c_val, z_sort);
  InsertZ3Expr(c_lit, z_val);

  return true;
}

void Z3ConvVisitor::VisitZ3Expr(z3::expr z_expr) {
  if (z_expr.is_app()) {
    for (auto i = 0U; i < z_expr.num_args(); ++i) {
      GetOrCreateCExpr(z_expr.arg(i));
    }
    switch (z_expr.decl().arity()) {
      case 0:
        VisitConstant(z_expr);
        break;

      case 1:
        VisitUnaryApp(z_expr);
        break;

      case 2:
        VisitBinaryApp(z_expr);
        break;

      default:
        LOG(FATAL) << "Unexpected Z3 operation!";
        break;
    }
  } else if (z_expr.is_quantifier()) {
    LOG(FATAL) << "Unexpected Z3 quantifier!";
  } else {
    LOG(FATAL) << "Unexpected Z3 variable!";
  }
}

void Z3ConvVisitor::VisitConstant(z3::expr z_const) {
  DLOG(INFO) << "VisitConstant: " << z_const;
  CHECK(z_const.is_const()) << "Z3 expression is not a constant!";
  // Create C literals and variable references
  auto kind = z_const.decl().decl_kind();
  clang::Expr *c_expr = nullptr;
  switch (kind) {
    // Boolean literals
    case Z3_OP_TRUE:
    case Z3_OP_FALSE:
    // Arithmetic numerals
    case Z3_OP_ANUM:
    // Bitvector numerals
    case Z3_OP_BNUM:
      c_expr = CreateLiteralExpr(z_const);
      break;
    // Internal constants handled by parent Z3 exprs
    case Z3_OP_INTERNAL:
      break;
    // Uninterpreted constants
    case Z3_OP_UNINTERPRETED:
      c_expr = CreateDeclRefExpr(*ast_ctx, GetCValDecl(z_const.decl()));
      break;

    // Unknowns
    default:
      LOG(FATAL) << "Unknown Z3 constant!";
      break;
  }
  InsertCExpr(z_const, c_expr);
}

void Z3ConvVisitor::VisitUnaryApp(z3::expr z_op) {
  DLOG(INFO) << "VisitUnaryApp: " << z_op;
  CHECK(z_op.is_app() && z_op.decl().arity() == 1)
      << "Z3 expression is not a unary operator!";
  // Get operand
  auto c_sub = GetCExpr(z_op.arg(0));
  auto t_sub = c_sub->getType();
  // Get z3 function declaration
  auto z_func = z_op.decl();
  // Create C unary operator
  clang::Expr *c_op = nullptr;
  switch (z_func.decl_kind()) {
    case Z3_OP_NOT:
      c_op = CreateNotExpr(*ast_ctx, c_sub);
      break;

    case Z3_OP_EXTRACT: {
      CHECK(t_sub->isIntegerType()) << "Extract operand is not an integer";
      auto s_size = GetZ3SortSize(z_op.get_sort());
      auto t_sign = t_sub->isSignedIntegerType();
      auto t_op = ast_ctx->getIntTypeForBitwidth(s_size, t_sign);
      c_op = CreateCStyleCastExpr(*ast_ctx, t_op,
                                  clang::CastKind::CK_IntegralCast, c_sub);
    } break;

    case Z3_OP_UNINTERPRETED: {
      // Resolve opcode
      auto z_func_name = z_func.name().str();
      if (z_func_name == "AddrOf") {
        auto t_op = ast_ctx->getPointerType(t_sub);
        c_op = CreateUnaryOperator(*ast_ctx, clang::UO_AddrOf, c_sub, t_op);
      } else if (z_func_name == "Deref") {
        CHECK(t_sub->isPointerType()) << "Deref operand type is not a pointer";
        auto t_op = t_sub->getPointeeType();
        c_op = CreateUnaryOperator(*ast_ctx, clang::UO_Deref, c_sub, t_op);
      } else if (z_func_name == "Paren") {
        c_op = CreateParenExpr(*ast_ctx, c_sub);
      } else if (z_func_name == "PtrDecay") {
        CHECK(t_sub->isArrayType()) << "PtrDecay operand type is not an array";
        auto t_op = ast_ctx->getArrayDecayedType(t_sub);
        c_op = CreateImplicitCastExpr(
            *ast_ctx, t_op, clang::CastKind::CK_ArrayToPointerDecay, c_sub);
      } else if (z_func_name == "PtrToInt") {
        auto s_size = GetZ3SortSize(z_op.get_sort());
        auto t_op = ast_ctx->getIntTypeForBitwidth(s_size, /*sign=*/0);
        c_op = CreateCStyleCastExpr(
            *ast_ctx, t_op, clang::CastKind::CK_PointerToIntegral, c_sub);
      } else {
        LOG(FATAL) << "Unknown Z3 uninterpreted function";
      }
    } break;

    default:
      LOG(FATAL) << "Unknown Z3 unary operator!";
      break;
  }
  // Save
  InsertCExpr(z_op, c_op);
}

void Z3ConvVisitor::VisitBinaryApp(z3::expr z_op) {
  DLOG(INFO) << "VisitBinaryApp: " << z_op;
  CHECK(z_op.is_app() && z_op.decl().arity() == 2)
      << "Z3 expression is not a binary operator!";
  // Get operands
  auto lhs = GetCExpr(z_op.arg(0));
  auto rhs = GetCExpr(z_op.arg(1));
  // Get result type for integers
  auto GetIntResultType = [this, &lhs, &rhs] {
    auto lht = lhs->getType();
    auto rht = rhs->getType();
    auto order = ast_ctx->getIntegerTypeOrder(lht, rht);
    return order < 0 ? rht : lht;
  };
  // Get z3 function declaration
  auto z_func = z_op.decl();
  // Create C binary operator
  clang::Expr *c_op = nullptr;
  switch (z_func.decl_kind()) {
    case Z3_OP_EQ:
      c_op = CreateBinaryOperator(*ast_ctx, clang::BO_EQ, lhs, rhs,
                                  ast_ctx->BoolTy);
      break;

    case Z3_OP_AND: {
      c_op = lhs;
      for (auto i = 1U; i < z_op.num_args(); ++i) {
        rhs = GetCExpr(z_op.arg(i));
        c_op = CreateBinaryOperator(*ast_ctx, clang::BO_LAnd, c_op, rhs,
                                    ast_ctx->BoolTy);
      }
    } break;

    case Z3_OP_OR: {
      c_op = lhs;
      for (auto i = 1U; i < z_op.num_args(); ++i) {
        rhs = GetCExpr(z_op.arg(i));
        c_op = CreateBinaryOperator(*ast_ctx, clang::BO_LOr, c_op, rhs,
                                    ast_ctx->BoolTy);
      }
    } break;

    case Z3_OP_BADD:
      c_op = CreateBinaryOperator(*ast_ctx, clang::BO_Add, lhs, rhs,
                                  GetIntResultType());
      break;

    case Z3_OP_BSREM:
    case Z3_OP_BSREM_I:
      c_op = CreateBinaryOperator(*ast_ctx, clang::BO_Rem, lhs, rhs,
                                  GetIntResultType());
      break;

    case Z3_OP_UNINTERPRETED: {
      auto name = z_func.name().str();
      // Resolve opcode
      if (name == "ArraySub") {
        auto base_type = lhs->getType()->getAs<clang::PointerType>();
        CHECK(base_type) << "Operand is not a clang::PointerType";
        c_op = CreateArraySubscriptExpr(*ast_ctx, lhs, rhs,
                                        base_type->getPointeeType());
      } else if (name == "Member") {
        auto mem = GetCValDecl(z_op.arg(1).decl());
        c_op = CreateMemberExpr(*ast_ctx, lhs, mem, mem->getType(),
                                /*is_arrow=*/false);
      } else if (name == "IntToPtr") {
        auto c_lit = clang::cast<clang::IntegerLiteral>(lhs);
        auto t_dst_opaque_ptr_val = c_lit->getValue().getLimitedValue();
        auto t_dst_opaque_ptr = reinterpret_cast<void *>(t_dst_opaque_ptr_val);
        auto t_dst = clang::QualType::getFromOpaquePtr(t_dst_opaque_ptr);
        c_op = CreateCStyleCastExpr(*ast_ctx, t_dst,
                                    clang::CastKind::CK_IntegralToPointer, rhs);
      } else {
        LOG(FATAL) << "Unknown Z3 uninterpreted function";
      }
    } break;

    // Unknowns
    default:
      LOG(FATAL) << "Unknown Z3 binary operator!";
      break;
  }
  // Save
  InsertCExpr(z_op, c_op);
}

}  // namespace rellic