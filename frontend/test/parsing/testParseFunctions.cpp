/*
 * Copyright 2021-2025 Hewlett Packard Enterprise Development LP
 * Other additional copyright holders may be indicated within.
 *
 * The entirety of this work is licensed under the Apache License,
 * Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.
 *
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

#include "test-parsing.h"

#include "chpl/parsing/Parser.h"
#include "chpl/framework/Context.h"
#include "chpl/uast/AstNode.h"
#include "chpl/uast/Call.h"
#include "chpl/uast/FnCall.h"
#include "chpl/uast/Formal.h"
#include "chpl/uast/Function.h"
#include "chpl/uast/Identifier.h"
#include "chpl/uast/Module.h"
#include "chpl/uast/OpCall.h"

static void test1(Parser* parser) {
  ErrorGuard guard(parser->context());
  auto parseResult = parseStringAndReportErrors(parser, "test1.chpl",
                                         "proc f() { }");
  assert(!guard.realizeErrors());
  auto mod = parseResult.singleModule();
  assert(mod);
  assert(mod->name().compare("test1") == 0);
  assert(mod->numStmts() == 1);
  auto functionDecl = mod->stmt(0)->toFunction();
  assert(functionDecl);
  assert(functionDecl->name().compare("f") == 0);
  assert(functionDecl->linkage() == Decl::DEFAULT_LINKAGE);
  assert(functionDecl->kind() == Function::PROC);
  assert(functionDecl->returnIntent() == Function::DEFAULT_RETURN_INTENT);
  assert(functionDecl->isInline() == false);
  assert(functionDecl->isOverride() == false);
  assert(functionDecl->throws() == false);
  assert(functionDecl->linkageName() == nullptr);
  assert(functionDecl->numFormals() == 0);
  assert(functionDecl->thisFormal() == nullptr);
  assert(functionDecl->returnType() == nullptr);
  assert(functionDecl->numLifetimeClauses() == 0);
  assert(functionDecl->numStmts() == 0);
}
static void test1a(Parser* parser) {
  ErrorGuard guard(parser->context());
  auto parseResult = parseStringAndReportErrors(parser, "test1a.chpl",
                                         "/* comment 1 */\n"
                                         "proc f() {\n"
                                         "  /* comment 2 */\n"
                                         "}\n"
                                         "/* comment 3 */\n");
  assert(!guard.realizeErrors());
  auto mod = parseResult.singleModule();
  assert(mod);
  assert(mod->name().compare("test1a") == 0);
  assert(mod->numStmts() == 3);
  assert(mod->stmt(0)->isComment());
  assert(mod->stmt(1)->isFunction());
  assert(mod->stmt(2)->isComment());
  auto functionDecl = mod->stmt(1)->toFunction();
  assert(functionDecl);
  assert(functionDecl->numStmts() == 1);
  assert(functionDecl->stmt(0)->isComment());
}
static void test1b(Parser* parser) {
  ErrorGuard guard(parser->context());
  auto parseResult = parseStringAndReportErrors(parser, "test1a.chpl",
                                         "/* comment 1 */\n"
                                         "proc f() {\n"
                                         "  /* comment 2 */\n"
                                         "  x;\n"
                                         "  /* comment 3 */\n"
                                         "}\n"
                                         "/* comment 4 */\n");
  assert(!guard.realizeErrors());
  auto mod = parseResult.singleModule();
  assert(mod);
  assert(mod->name().compare("test1a") == 0);
  assert(mod->numStmts() == 3);
  assert(mod->stmt(0)->isComment());
  assert(mod->stmt(1)->isFunction());
  assert(mod->stmt(2)->isComment());
  auto functionDecl = mod->stmt(1)->toFunction();
  assert(functionDecl);
  assert(functionDecl->numStmts() == 3);
  assert(functionDecl->stmt(0)->isComment());
  assert(functionDecl->stmt(1)->isIdentifier());
  assert(functionDecl->stmt(0)->isComment());
}

static void test1c(Parser* parser) {
  ErrorGuard guard(parser->context());
  auto parseResult = parseStringAndReportErrors(parser, "test1c.chpl",
                                         "/* comment 1 */\n"
                                         "public inline\n"
                                         "proc f() {\n"
                                         "  x;\n"
                                         "}\n");
  assert(!guard.realizeErrors());
  auto mod = parseResult.singleModule();
  assert(mod);
  assert(mod->name().compare("test1c") == 0);
  assert(mod->numStmts() == 2);
  assert(mod->stmt(0)->isComment());
  assert(mod->stmt(1)->isFunction());
  auto functionDecl = mod->stmt(1)->toFunction();
  assert(functionDecl);
  assert(functionDecl->numStmts() == 1);
  assert(functionDecl->stmt(0)->isIdentifier());
}

static void test1d(Parser* parser) {
  ErrorGuard guard(parser->context());
  auto parseResult = parseStringAndReportErrors(parser, "test1d.chpl",
                                         "public\n"
                                         "/* comment 1 */\n"
                                         "inline\n"
                                         "proc f() {\n"
                                         "  x;\n"
                                         "}\n");
  assert(!guard.realizeErrors());
  auto mod = parseResult.singleModule();
  assert(mod);
  assert(mod->name().compare("test1d") == 0);
  assert(mod->numStmts() == 1);
  assert(mod->stmt(0)->isFunction());
  auto functionDecl = mod->stmt(0)->toFunction();
  assert(functionDecl);
  assert(functionDecl->numStmts() == 1);
  assert(functionDecl->stmt(0)->isIdentifier());
}

static void test1e(Parser* parser) {
  ErrorGuard guard(parser->context());
  auto parseResult = parseStringAndReportErrors(parser, "test1e.chpl",
                                         "public\n"
                                         "inline\n"
                                         "/* comment 1 */\n"
                                         "proc f() {\n"
                                         "  x;\n"
                                         "}\n");
  assert(!guard.realizeErrors());
  auto mod = parseResult.singleModule();
  assert(mod);
  assert(mod->name().compare("test1e") == 0);
  assert(mod->numStmts() == 1);
  assert(mod->stmt(0)->isFunction());
  auto functionDecl = mod->stmt(0)->toFunction();
  assert(functionDecl);
  assert(functionDecl->numStmts() == 1);
  assert(functionDecl->stmt(0)->isIdentifier());
}

static void test1f(Parser* parser) {
  ErrorGuard guard(parser->context());
  auto parseResult = parseStringAndReportErrors(parser, "test1f.chpl",
                                         "proc f(/* comment 1 */) {\n"
                                         "  x;\n"
                                         "}\n");
  assert(!guard.realizeErrors());
  auto mod = parseResult.singleModule();
  assert(mod);
  assert(mod->name().compare("test1f") == 0);
  assert(mod->numStmts() == 1);
  assert(mod->stmt(0)->isFunction());
  auto functionDecl = mod->stmt(0)->toFunction();
  assert(functionDecl);
  assert(functionDecl->numStmts() == 1);
  assert(functionDecl->stmt(0)->isIdentifier());
}

static void test1g(Parser* parser) {
  ErrorGuard guard(parser->context());
  auto parseResult = parseStringAndReportErrors(parser, "test1g.chpl",
                                         "proc f()\n"
                                         "/* comment 1 */ where a \n"
                                         "{\n"
                                         "  x;\n"
                                         "}\n");
  assert(!guard.realizeErrors());
  auto mod = parseResult.singleModule();
  assert(mod);
  assert(mod->name().compare("test1g") == 0);
  assert(mod->numStmts() == 1);
  assert(mod->stmt(0)->isFunction());
  auto functionDecl = mod->stmt(0)->toFunction();
  assert(functionDecl);
  assert(functionDecl->numStmts() == 1);
  assert(functionDecl->stmt(0)->isIdentifier());
}

static void test1h(Parser* parser) {
  ErrorGuard guard(parser->context());
  auto parseResult = parseStringAndReportErrors(parser, "test1h.chpl",
                                         "proc f()\n"
                                         "where a /* comment 1 */\n"
                                         "{\n"
                                         "  x;\n"
                                         "}\n");
  assert(!guard.realizeErrors());
  auto mod = parseResult.singleModule();
  assert(mod);
  assert(mod->name().compare("test1h") == 0);
  assert(mod->numStmts() == 1);
  assert(mod->stmt(0)->isFunction());
  auto functionDecl = mod->stmt(0)->toFunction();
  assert(functionDecl);
  assert(functionDecl->numStmts() == 1);
  assert(functionDecl->stmt(0)->isIdentifier());
}

static void test1i(Parser* parser) {
  ErrorGuard guard(parser->context());
  auto parseResult = parseStringAndReportErrors(parser, "test1i.chpl",
                                         "proc f()\n"
                                         "lifetime /* comment 1 */ a=b\n"
                                         "{\n"
                                         "  x;\n"
                                         "}\n");
  assert(!guard.realizeErrors());
  auto mod = parseResult.singleModule();
  assert(mod);
  assert(mod->name().compare("test1i") == 0);
  assert(mod->numStmts() == 1);
  assert(mod->stmt(0)->isFunction());
  auto functionDecl = mod->stmt(0)->toFunction();
  assert(functionDecl);
  assert(functionDecl->numStmts() == 1);
  assert(functionDecl->stmt(0)->isIdentifier());
}

static void test1j(Parser* parser) {
  ErrorGuard guard(parser->context());
  auto parseResult = parseStringAndReportErrors(parser, "test1j.chpl",
                                         "proc f()\n"
                                         "lifetime a=b /* comment 1 */\n"
                                         "{\n"
                                         "  x;\n"
                                         "}\n");
  assert(!guard.realizeErrors());
  auto mod = parseResult.singleModule();
  assert(mod);
  assert(mod->name().compare("test1j") == 0);
  assert(mod->numStmts() == 1);
  assert(mod->stmt(0)->isFunction());
  auto functionDecl = mod->stmt(0)->toFunction();
  assert(functionDecl);
  assert(functionDecl->numStmts() == 1);
  assert(functionDecl->stmt(0)->isIdentifier());
}






static BuilderResult parseFunction(Parser* parser,
                                   const char* filename,
                                   const Function*& f,
                                   const char* contents) {
  ErrorGuard guard(parser->context());
  auto parseResult = parseStringAndReportErrors(parser, filename, contents);
  assert(!guard.realizeErrors());
  auto mod = parseResult.singleModule();
  assert(mod);
  assert(mod->numStmts() == 1);
  auto functionDecl = mod->stmt(0)->toFunction();
  assert(functionDecl);
  f = functionDecl;
  return parseResult;
}

static void test2(Parser* parser) {
  ErrorGuard guard(parser->context());
  const Function* function = nullptr;
  auto parse = parseFunction(
      parser,
      "test2.chpl",
      function,
      "inline proc f(a: int) ref { x; }");

  assert(function->name().compare("f") == 0);
  assert(function->linkage() == Decl::DEFAULT_LINKAGE);
  assert(function->kind() == Function::PROC);
  assert(function->returnIntent() == Function::REF);
  assert(function->isInline() == true);
  assert(function->isOverride() == false);
  assert(function->throws() == false);
  assert(function->linkageName() == nullptr);
  assert(function->numFormals() == 1);
  auto formalDecl = function->formal(0);
  assert(formalDecl);
  auto formal = formalDecl->toFormal();
  assert(formal);
  assert(formal->intent() == Formal::DEFAULT_INTENT);
  assert(formal->name().compare("a") == 0);
  auto typeExpr = formal->typeExpression();
  assert(typeExpr);
  assert(typeExpr->isIdentifier());
  assert(typeExpr->toIdentifier()->name().compare("int") == 0);
  assert(formal->initExpression() == nullptr);
  assert(function->thisFormal() == nullptr);
  assert(function->returnType() == nullptr);
  assert(function->numLifetimeClauses() == 0);
  assert(function->numStmts() == 1);
  auto stmt = function->stmt(0);
  assert(stmt);
  assert(stmt->isIdentifier());
  assert(stmt->toIdentifier()->name().compare("x") == 0);
}

static void test3(Parser* parser) {
  ErrorGuard guard(parser->context());
  const Function* function = nullptr;
  auto parse = parseFunction(
      parser,
      "test3.chpl",
      function,
      "override proc const R.f(ref a: int = b) const ref { }");
  assert(function->name().compare("f") == 0);
  assert(function->linkage() == Decl::DEFAULT_LINKAGE);
  assert(function->kind() == Function::PROC);
  assert(function->returnIntent() == Function::CONST_REF);
  assert(function->isInline() == false);
  assert(function->isOverride() == true);
  assert(function->throws() == false);
  assert(function->linkageName() == nullptr);
  assert(function->numFormals() == 2); // 'this' and 'a'

  auto thisFormalDecl = function->formal(0);
  assert(thisFormalDecl);
  auto thisFormal = thisFormalDecl->toFormal();
  assert(thisFormal);
  assert(thisFormal->intent() == Formal::CONST);
  assert(thisFormal->name().compare("this") == 0);
  auto thisTypeExpr = thisFormal->typeExpression();
  assert(thisTypeExpr);
  assert(thisTypeExpr->isIdentifier());
  assert(thisTypeExpr->toIdentifier()->name().compare("R") == 0);
  assert(thisFormal->initExpression() == nullptr);

  auto formalDecl = function->formal(1);
  assert(formalDecl);
  auto formal = formalDecl->toFormal();
  assert(formal);
  assert(formal->intent() == Formal::REF);
  assert(formal->name().compare("a") == 0);
  auto typeExpr = formal->typeExpression();
  assert(typeExpr);
  assert(typeExpr->isIdentifier());
  assert(typeExpr->toIdentifier()->name().compare("int") == 0);
  auto initExpr = formal->initExpression();
  assert(initExpr);
  assert(initExpr->isIdentifier());
  assert(initExpr->toIdentifier()->name().compare("b") == 0);

  assert(function->thisFormal() == thisFormal);
  assert(function->returnType() == nullptr);
  assert(function->numLifetimeClauses() == 0);
  assert(function->numStmts() == 0);
}

static void test4(Parser* parser) {
  ErrorGuard guard(parser->context());
  const Function* function = nullptr;
  auto parse = parseFunction(
      parser,
      "test4.chpl",
      function,
      "proc f(a: int, b = x) ref { y; z; }");

  // this test is focused on checking the function iterators
  assert(function->name().compare("f") == 0);
  assert(function->numFormals() == 2);
  auto formalA = function->formal(0);
  assert(formalA);
  auto formalB = function->formal(1);
  assert(formalB);
  assert(function->numStmts() == 2);
  auto bodyY = function->stmt(0);
  assert(bodyY);
  auto bodyZ = function->stmt(1);
  assert(bodyZ);

  // Now check the various alternative access methods.
  assert(function->formal(0) == formalA);
  assert(function->formal(1) == formalB);

  int i;

  i = 0;
  for (auto formal : function->formals()) {
    if (i == 0) assert(formal == formalA);
    if (i == 1) assert(formal == formalB);
    i++;
  }
}


static void test5(Parser* p) {
  ErrorGuard guard(p->context());
  auto parseResult = p->parseString("test5.chpl",
                                    "private extern proc hello();\n");
  assert(!guard.realizeErrors());
  auto mod = parseResult.singleModule();
  assert(mod);
  assert(mod->numStmts()==1);
  auto fn = mod->stmt(0)->toFunction();
  assert(fn);
  assert(fn->visibility() == Decl::PRIVATE);
  assert(fn->linkage() == Decl::EXTERN);
  assert(fn->numStmts() == 0);
  assert(fn->numFormals() == 0);
}


int main() {
  Context context;
  Context* ctx = &context;

  auto parser = Parser::createForTopLevelModule(ctx);
  Parser* p = &parser;

  test1(p);
  test1a(p);
  test1b(p);
  test1c(p);
  test1d(p);
  test1e(p);
  test1f(p);
  test1g(p);
  test1h(p);
  test1i(p);
  test1j(p);
  test2(p);
  test3(p);
  test4(p);
  test5(p);

  return 0;
}
