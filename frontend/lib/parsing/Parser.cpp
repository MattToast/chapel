/*
 * Copyright 2020-2025 Hewlett Packard Enterprise Development LP
 * Copyright 2004-2019 Cray Inc.
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

#include "chpl/parsing/Parser.h"

#include "../util/filesystem_help.h"

#include "chpl/framework/ErrorMessage.h"
#include "chpl/framework/ErrorBase.h"
#include "chpl/uast/AstNode.h"
#include "chpl/uast/Comment.h"

#include "./bison-chpl-lib.h"
#include "./flex-chpl-lib.h"

#include <cstdlib>
#include <cstring>
#include <tuple>

#define DEBUG_PARSER 0

namespace chpl {
namespace parsing {


using namespace chpl::uast;

Parser::Parser(Context* context, UniqueString parentSymbolPath)
  : context_(context), parentSymbolPath_(parentSymbolPath) {
}

Parser Parser::createForTopLevelModule(Context* context) {
  UniqueString emptySymbolPath;
  return Parser(context, emptySymbolPath);
}

Parser Parser::createForIncludedModule(Context* context,
                                       UniqueString parentSymbolPath) {
  return Parser(context, parentSymbolPath);
}

static void updateParseResult(ParserContext* parserContext) {

  Builder* builder = parserContext->builder;

  // Save the top-level exprs
  if (parserContext->topLevelStatements != nullptr) {
    for (AstNode* stmt : *parserContext->topLevelStatements) {
      builder->addToplevelExpression(toOwned(stmt));
    }
    delete parserContext->topLevelStatements;
  }
  // Save any remaining top-level comments
  if (parserContext->comments != nullptr) {
    for (ParserComment parserComment : *parserContext->comments) {
      builder->addToplevelExpression(toOwned(parserComment.comment));
    }
    delete parserContext->comments;
  }
}


BuilderResult Parser::parseFile(const char* path, ParserStats* parseStats) {
  owned<Builder> builder;
  if (parentSymbolPath_.isEmpty()) {
    builder = Builder::createForTopLevelModule(this->context(), path);
  } else {
    builder = Builder::createForIncludedModule(this->context(), path,
                                               parentSymbolPath_);
  }
  std::string fileError;

  FILE* fp = openfile(path, "r", fileError);
  if (fp == NULL) {
    context_->report(GeneralError::error(Location(), fileError));
    return builder->result();
  }

  // Otherwise, we have successfully opened the file.

  // Set the (global) parser debug state
  if (DEBUG_PARSER)
    yychpl_debug = DEBUG_PARSER;

  // State for the lexer
  int           lexerStatus  = 100;

  // State for the parser
  yychpl_pstate* parser = yychpl_pstate_new();
  int           parserStatus = YYPUSH_MORE;
  YYLTYPE       my_yylloc;
  ParserContext parserContext(path, builder.get(), parseStats);

  my_yylloc.first_line             = 1;
  my_yylloc.first_column           = 1;
  my_yylloc.last_line              = 1;
  my_yylloc.last_column            = 1;

  yychpl_lex_init_extra(&parserContext, &parserContext.scanner);

  yychpl_set_in(fp, parserContext.scanner);

  while (parserStatus == YYPUSH_MORE) {
    YYSTYPE my_yylval;

    // In some situations, the parser context may have set 'atEOF' before
    // the parser has seen EOF. The lexer will have already produced the
    // EOF token in this case. The below check prevents the lexer from
    // trying to swap to a nonexistent buffer.
    if (!parserContext.atEOF) {
      lexerStatus = yychpl_lex(&my_yylval, &my_yylloc, parserContext.scanner);
    } else {
      lexerStatus = 0;
    }

    if (lexerStatus >= 0) {
      parserStatus          = yychpl_push_parse(parser,
                                                lexerStatus,
                                                &my_yylval,
                                                &my_yylloc,
                                                &parserContext);

    } else if (lexerStatus == YYLEX_BLOCK_COMMENT) {
      // comment should already be noted in processBlockComment
    } else if (lexerStatus == YYLEX_SINGLE_LINE_COMMENT) {
      // comment should already be noted in processSingleLineComment

      // Single line comments may cause the parser context to set 'atEOF'
      // before the parser has registered that EOF occurred (e.g. for a
      // single line comment followed immediately by EOF). In this case,
      // complete one more iteration of the loop instead of breaking.
      if (parserContext.atEOF) continue;
    }

    if (lexerStatus == 0 || parserContext.atEOF)
      break;
  }

  // Cleanup after the parser
  yychpl_pstate_delete(parser);

  // Cleanup after the lexer
  yychpl_lex_destroy(parserContext.scanner);

  if (closefile(fp, path, fileError)) {
    context_->report(GeneralError::error(Location(), fileError));
  }

  updateParseResult(&parserContext);

  // compute the result from the builder
  return builder->result();
}


BuilderResult Parser::parseString(const char* path, const char* str,
                                  ParserStats* parseStats) {
  owned<Builder> builder;
  if (parentSymbolPath_.isEmpty()) {
    builder = Builder::createForTopLevelModule(this->context(), path);
  } else {
    builder = Builder::createForIncludedModule(this->context(), path,
                                               parentSymbolPath_);
  }

  // Set the (global) parser debug state
  if (DEBUG_PARSER)
    yychpl_debug = DEBUG_PARSER;

  // State for the lexer
  YY_BUFFER_STATE handle       =   0;
  int             lexerStatus  = 100;
  YYLTYPE         my_yylloc;

  // State for the parser
  yychpl_pstate* parser = yychpl_pstate_new();
  int           parserStatus = YYPUSH_MORE;
  ParserContext parserContext(path, builder.get(), parseStats);

  yychpl_lex_init_extra(&parserContext, &parserContext.scanner);

  handle = yychpl__scan_string(str, parserContext.scanner);

  my_yylloc.first_line   = 1;
  my_yylloc.first_column = 1;
  my_yylloc.last_line    = 1;
  my_yylloc.last_column  = 1;

  while (parserStatus == YYPUSH_MORE) {
    YYSTYPE my_yylval;

    // In some situations, the parser context may have set 'atEOF' before
    // the parser has seen EOF. The lexer will have already produced the
    // EOF token in this case. The below check prevents the lexer from
    // trying to swap to a nonexistent buffer.
    if (!parserContext.atEOF) {
      lexerStatus = yychpl_lex(&my_yylval, &my_yylloc, parserContext.scanner);
    } else {
      lexerStatus = 0;
    }

    if (lexerStatus >= 0) {
      parserStatus          = yychpl_push_parse(parser,
                                                lexerStatus,
                                                &my_yylval,
                                                &my_yylloc,
                                                &parserContext);

    } else if (lexerStatus == YYLEX_BLOCK_COMMENT) {
      // comment should already be noted in processBlockComment
    } else if (lexerStatus == YYLEX_SINGLE_LINE_COMMENT) {
      // comment should already be noted in processSingleLineComment

      // Single line comments may cause the parser context to set 'atEOF'
      // before the parser has registered that EOF occurred (e.g. for a
      // single line comment followed immediately by EOF). In this case,
      // complete one more iteration of the loop instead of breaking.
      if (parserContext.atEOF) continue;
    }

    if (lexerStatus == 0 || parserContext.atEOF)
      break;
  }

  // Cleanup after the parser
  yychpl_pstate_delete(parser);

  // Cleanup after the lexer
  yychpl__delete_buffer(handle, parserContext.scanner);
  yychpl_lex_destroy(parserContext.scanner);

  updateParseResult(&parserContext);

  return builder->result();
}


} // namespace parsing
} // namespace chpl
