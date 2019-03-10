/// \file Parser for SMT-LIB2 generated by Yosys
/// the use of it is
///   1. translate data-type to bitvector declarations
///     a. datatype : recursively defined...
///     b. fix some 
///   2. add a no-change transition function
///   3. -- (currently no ) change variable name to their meaningful names

#include <ilang/util/log.h>
#include <ilang/util/str_util.h>
#include <ilang/util/container_shortcut.h>
#include <ilang/smt-inout/yosys_smt_parser.h>

namespace ilang {
namespace smt {


/// construct flatten_datatype (hierarchically)
void YosysSmtParser::construct_flatten_dataype() {
  for (auto && module_name : smt_ast.data_type_order) {
    ILA_ASSERT(IN(module_name, smt_ast.datatypes));
    auto & state_var_vec = smt_ast.datatypes[module_name];
    { // create the same module there
      // one by one insert the elements
      // check if it is Datatype
      // find in the flatten_one, it must exist
      // copy all its var here
      ILA_ASSERT(not IN(module_name, flatten_datatype));
      for (const auto & state_var : state_var_vec) {
        if(state_var._type._type == var_type::tp::Datatype) {
          const auto & mod_tp_name = state_var._type.module_name;
          auto tp_mod_name = mod_tp_name.substr(1, mod_tp_name.length()-4); // | _s|
          ILA_ASSERT(IN(tp_mod_name, flatten_datatype));
          auto & sub_mod_state_var_vec = flatten_datatype[tp_mod_name];
          // from there, insert all the state here
          for(const auto & sub_mod_state_var : sub_mod_state_var_vec) {
            flatten_datatype[module_name].push_back(sub_mod_state_var);
          }
        } // if itself is a datatype
        else
          flatten_datatype[module_name].push_back(state_var);
      } // for all state_vars under this module name 
    } // end of create the same module there in flatten_...
  } // for all module in order
} // construct_flatten_dataype

/// replace function body and argument 
void YosysSmtParser::replace_function_arg_body() {
  // now we have flatten_datatype for reference
  // a function should only refer to a mod's own datatype
  // prepare a replace map for it 
  // (string --> string)
  // ((state |mod_s|)) -> (flattened, but var name)
    // arg_name, arg_type
  // (|??| state) -> internal_name
  std::string cached_func_module; // ""
  std::string cached_arg_text;
  std::string cached_arg_replace;
  std::vector<arg_t> cached_args;
  std::map<std::string,std::string> cached_body_replace;

  for (auto && one_smt_item_ptr : smt_ast.items) {
    std::shared_ptr<func_def_t> fn = 
      std::dynamic_pointer_cast<func_def_t>(one_smt_item_ptr);
    if (not fn) // only handle functions
      continue; 
    ILA_ASSERT(not fn->func_module.empty());
    if( fn->func_module != cached_func_module) { // re-cached
      cached_func_module = fn->func_module;
      cached_arg_text = "((state |" + cached_func_module+"_s|)";
      { // for all state (flattened), add to arg and also cached_body_replace
        ILA_ASSERT(IN(cached_func_module, flatten_datatype));
        auto & all_flattened_state_var = flatten_datatype[cached_func_module];
        
      }
      cached_arg_replace = "(" + toString(cached_args) + ")";
    } // end of re-cache
  } // for one_smt_item_ptr
} // replace_function_arg_body
/// add the no-change-function (hierarchically)
void YosysSmtParser::add_no_change_function() {

}

// -------------- CONSTRUCTOR -------------------- //
YosysSmtParser::YosysSmtParser(const std::string & buf) {
  // parse from string
  str_iterator iter(buf);
  ParseFromString(iter, smt_ast);
}

}; // namespace smt
}; // namespace ilang