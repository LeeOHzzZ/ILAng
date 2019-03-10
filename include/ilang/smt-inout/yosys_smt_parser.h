/// \file Parser for SMT-LIB2 generated by Yosys
/// the use of it is
///   1. translate data-type to bitvector declarations
///   2. add a no-change transition function
///   3. change variable name to their meaningful names

#ifndef YOSYS_SMT_PARSER_H__
#define YOSYS_SMT_PARSER_H__

#include <ilang/smt-inout/smt_ast.h>

#include <iostream>
#include <string>

namespace ilang {
namespace smt {

/// \brief to parse an smt file
/// this will only work on the yosys's generated smt
/// and not on the assemblied CHC
class YosysSmtParser {
  // ---------------- TYPE DEFs -------------------- //
protected:
  /// the internal smt-ast
  smt_file smt_ast;
  /// the datatype defs without datatypes
  datatypes_t flatten_datatype;
  // ------------- HELPER FUNCTIONS ---------------- //
  /// construct flatten_datatype (hierarchically)
  void construct_flatten_dataype();
  /// replace function body and argument 
  void replace_function_arg_body();
  /// add the no-change-function (hierarchically)
  void add_no_change_function();
public:
  // -------------- CONSTRUCTOR -------------------- //
  YosysSmtParser(const std::string & buf);
  /// 
  
}; // class YosysSmtParser

}; // namespace smt
}; // namespace ilang

#endif