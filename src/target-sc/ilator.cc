/// \file
/// Implementation of the class Ilator.

#include <fstream>

#include <fmt/format.h>

#include <ilang/ila-mngr/u_abs_knob.h>
#include <ilang/ila/ast_fuse.h>
#include <ilang/util/fs.h>
#include <ilang/util/log.h>

#include <ilang/target-sc/ilator.h>

/// \namespace ilang
namespace ilang {

//
// static helpers/members
//

static const std::string dir_app = "app";
static const std::string dir_src = "src";
static const std::string dir_include = "include";
static const std::string dir_extern = "extern";

static std::unordered_map<size_t, size_t> pivotal_id;

size_t GetPivotalId(const size_t& id) {
  if (auto pos = pivotal_id.find(id); pos == pivotal_id.end()) {
    auto new_id = pivotal_id.size();
    pivotal_id.insert({id, new_id});
    return new_id;
  } else {
    return pos->second;
  }
}

void WriteFile(const std::string& file_path, const fmt::memory_buffer& buff) {
  std::ofstream fw(file_path);
  ILA_ASSERT(fw.is_open()) << "Fail opening file " << file_path;
  fw << to_string(buff);
  fw.close();
}

bool HasLoadFromStore(const ExprPtr& expr) {
  auto monitor = false;
  auto LoadFromStore = [&monitor](const ExprPtr& e) {
    if (e->is_op()) {
      if (auto uid = GetUidExprOp(e); uid == AST_UID_EXPR_OP::LOAD) {
        monitor |= e->arg(0)->is_op();
      }
    }
  };
  expr->DepthFirstVisit(LoadFromStore);
  return monitor;
}

//
// Ilator implementation
//

Ilator::Ilator(const InstrLvlAbsPtr& m) : m_(m) {}

Ilator::~Ilator() { Reset(); }

void Ilator::Generate(const std::string& dst) {
  // sanity checks and initialize
  if (!SanityCheck() || !Bootstrap(dst)) {
    return;
  }

  auto status = true;
  ILA_INFO << "Start generating SystemC simulator of " << m_;

  // instruction semantics (decode and updates)
  for (auto& instr : AbsKnob::GetInstrTree(m_)) {
    status &= GenerateInstrContent(instr, os_portable_append_dir(dst, dir_src));
  }

  // memory updates
  status &= GenerateMemoryUpdate(os_portable_append_dir(dst, dir_src));

  // constant memory
  status &= GenerateConstantMemory(os_portable_append_dir(dst, dir_src));

  // execution kernel
  status &= GenerateExecuteKernel(os_portable_append_dir(dst, dir_src));

  // shared header (input, state, func., etc.)
  status &= GenerateGlobalHeader(os_portable_append_dir(dst, dir_include));

  // cmake support, e.g., recipe and templates
  status &= GenerateBuildSupport(dst);

  // clean up if something went wrong
  if (status) {
    ILA_INFO << "Sucessfully generate SystemC simulator at " << dst;
  } else {
    ILA_ERROR << "Fail generating simulator at " << dst;
#ifdef NDEBUG
    os_portable_remove_directory(dst);
#endif // NDEBUG
  }
}

void Ilator::Reset() {
  // functions
  for (auto f : functions_) {
    delete f.second;
  }
  functions_.clear();

  // externs
  for (auto f : externs_) {
    delete f.second;
  }
  externs_.clear();

  // memory updates
  for (auto f : memory_updates_) {
    delete f.second;
  }
  memory_updates_.clear();

  // others
  source_files_.clear();
  const_mems_.clear();
  global_vars_.clear();
}

bool Ilator::SanityCheck() const {
  // add new check here
  return true;
}

bool Ilator::Bootstrap(const std::string& root) {
  Reset();

  // create/structure project directory
  auto res_mkdir = os_portable_mkdir(root);
  res_mkdir &= os_portable_mkdir(os_portable_append_dir(root, dir_app));
  res_mkdir &= os_portable_mkdir(os_portable_append_dir(root, dir_extern));
  res_mkdir &= os_portable_mkdir(os_portable_append_dir(root, dir_include));
  res_mkdir &= os_portable_mkdir(os_portable_append_dir(root, dir_src));
  if (!res_mkdir) {
    os_portable_remove_directory(root);
    ILA_ERROR << "Fail structuring simulator direcory at " << root;
    return false;
  }
  return true;
}

bool Ilator::GenerateInstrContent(const InstrPtr& instr,
                                  const std::string& dir) {
  fmt::memory_buffer buff;
  ExprVarMap lut;

  // include headers
  fmt::format_to(buff, "#include <{}.h>\n", GetProjectName());

  // decode function
  auto decode_expr = instr->decode();
  auto decode_func = RegisterFunction(GetDecodeFuncName(instr), decode_expr);
  BeginFuncDef(decode_func, buff);
  lut.clear();
  if (!RenderExpr(decode_expr, buff, lut)) {
    return false;
  }
  fmt::format_to(buff, "auto& {universal_name} = {local_name};\n",
                 fmt::arg("universal_name", GetCxxName(decode_expr)),
                 fmt::arg("local_name", LookUp(decode_expr, lut)));
  EndFuncDef(decode_func, buff);

  // next state
  auto update_func = RegisterFunction(GetUpdateFuncName(instr));
  BeginFuncDef(update_func, buff);
  lut.clear();
  auto updated_states = instr->updated_states();
  for (auto& s : updated_states) {
    if (auto update_expr = instr->update(s); !update_expr->is_mem()) {
      if (!RenderExpr(update_expr, buff, lut)) {
        return false;
      }
      fmt::format_to(buff, "auto {next_holder} = {next_internal};\n",
                     fmt::arg("next_holder", GetCxxName(update_expr)),
                     fmt::arg("next_internal", LookUp(update_expr, lut)));
    } else { // memory (one copy for performance, need special handling)
      if (HasLoadFromStore(update_expr)) {
        return false;
      }
      auto placeholder = GetLocalVar(lut);
      auto [it, status] = lut.insert({update_expr, placeholder});
      ILA_ASSERT(status);
      auto mem_update_func = RegisterMemoryUpdate(update_expr);
      fmt::format_to(buff,
                     "{mem_type} {placeholder};\n"
                     "{mem_update_func}({placeholder});\n",
                     fmt::arg("mem_type", GetCxxType(update_expr)),
                     fmt::arg("mem_update_func", mem_update_func->name),
                     fmt::arg("placeholder", placeholder));
      // dummy traverse collect related memory operation
      fmt::memory_buffer dummy_buff;
      ExprVarMap dummy_lut;
      if (!RenderExpr(update_expr, dummy_buff, dummy_lut)) {
        return false;
      }
    }
  }
  // update state
  for (auto& s : updated_states) {
    auto curr = instr->host()->state(s);
    auto next = instr->update(s);
    if (!curr->is_mem()) {
      fmt::format_to(buff, "{current} = {next_value};\n",
                     fmt::arg("current", GetCxxName(curr)),
                     fmt::arg("next_value", GetCxxName(next)));
    } else {
      fmt::format_to(buff,
                     "for (auto& it : {next_value}) {{\n"
                     "  {current}[it.first] = it.second;\n"
                     "}}\n",
                     fmt::arg("current", GetCxxName(curr)),
                     fmt::arg("next_value", LookUp(next, lut)));
    }
  }
  EndFuncDef(update_func, buff);

  // record and write to file
  CommitSource(fmt::format("idu_{}.cc", instr->name().str()), dir, buff);
  return true;
}

bool Ilator::GenerateMemoryUpdate(const std::string& dir) {
  fmt::memory_buffer buff;
  ExprVarMap lut;

  int file_cnt = 0;
  auto GetMemUpdateFile = [&file_cnt]() {
    return fmt::format("memory_update_functions_{}.cc", file_cnt++);
  };

  auto StartNewFile = [this, &buff]() {
    buff.clear();
    fmt::format_to(buff, "#include <{}.h>\n", GetProjectName());
  };

  StartNewFile();

  auto status = true;
  for (auto mem_update_func_it : memory_updates_) {
    auto& mem_update_func = mem_update_func_it.second;
    ILA_NOT_NULL(mem_update_func);
    auto& mem = mem_update_func->target;

    lut.clear();

    BeginFuncDef(mem_update_func, buff);

    if (auto uid = GetUidExprOp(mem); uid == AST_UID_EXPR_OP::STORE) {
      status &= RenderExpr(mem, buff, lut);
    } else { // ite
      status &= RenderExpr(mem->arg(0), buff, lut);
      fmt::format_to(buff, "if ({}) {{\n", LookUp(mem->arg(0), lut));
      auto lut_local_0 = lut;
      status &= RenderExpr(mem->arg(1), buff, lut_local_0);
      fmt::format_to(buff, "}} else {{\n");
      status &= RenderExpr(mem->arg(2), buff, lut); // reuse lut
      fmt::format_to(buff, "}}\n");
    }

    EndFuncDef(mem_update_func, buff);

    if (buff.size() > 100000) {
      CommitSource(GetMemUpdateFile(), dir, buff);
      StartNewFile();
    }
    if (!status) {
      return status;
    }
  }

  CommitSource(GetMemUpdateFile(), dir, buff);
  return status;
}

bool Ilator::GenerateConstantMemory(const std::string& dir) {
  fmt::memory_buffer buff;
  fmt::format_to(buff, "#include <{}.h>\n", GetProjectName());

  for (auto& mem : const_mems_) {
    auto const_mem = std::dynamic_pointer_cast<ExprConst>(mem);
    const auto& val_map = const_mem->val_mem()->val_map();
    std::vector<std::string> addr_data_pairs;
    for (auto& it : val_map) {
      addr_data_pairs.push_back(fmt::format("  {{{addr}, {data}}}",
                                            fmt::arg("addr", it.first),
                                            fmt::arg("data", it.second)));
    }
    fmt::format_to(
        buff,
        "{var_type} {project}::{var_name} = {{\n"
        "{addr_data_pairs}\n"
        "}};\n",
        fmt::arg("var_type", GetCxxType(mem)),
        fmt::arg("project", GetProjectName()),
        fmt::arg("var_name", GetCxxName(mem)),
        fmt::arg("addr_data_pairs", fmt::join(addr_data_pairs, ",\n")));
  }

  CommitSource("constant_memory_def.cc", dir, buff);
  return true;
}

bool Ilator::GenerateExecuteKernel(const std::string& dir) {
  auto kernel_func = RegisterFunction("compute");
  fmt::memory_buffer buff;

  fmt::format_to( // headers
      buff,
      "#include <iomanip>\n"
      "#include <{project}.h>\n",
      fmt::arg("project", GetProjectName()));

  fmt::format_to( // logging
      buff,
      "static int instr_cntr = 0;\n"
      "void {project}::LogInstrSequence(const std::string& instr_name) {{\n"
      "  instr_log << \"Instr No.\" << std::setw(5) << instr_cntr;\n"
      "  instr_log << instr_name << \" is activated\\n\";\n"
      "  instr_cntr++;\n"
      "}}\n",
      fmt::arg("project", GetProjectName()));

  BeginFuncDef(kernel_func, buff);

  // read in input value
  for (size_t i = 0; i < m_->input_num(); i++) {
    fmt::format_to(buff, "{input_name} = {input_name}_in.read();\n",
                   fmt::arg("input_name", GetCxxName(m_->input(i))));
  }

  // init TODO

  // instruction execution
  auto ExecInstr = [this, &buff](const InstrPtr& instr, bool child) {
    fmt::format_to(
        buff,
        "if ({decode_func_name}()) {{\n"
        "  {update_func_name}();\n"
        "  {child_counter}"
        "#ifdef ILATOR_VERBOSE\n"
        "  LogInstrSequence(\"{instr_name}\");\n"
        "#endif\n"
        "}}\n",
        fmt::arg("decode_func_name", GetDecodeFuncName(instr)),
        fmt::arg("update_func_name", GetUpdateFuncName(instr)),
        fmt::arg("child_counter", (child ? "schedule_counter++;\n" : "")),
        fmt::arg("instr_name", instr->name().str()));
  };

  auto top_instrs = AbsKnob::GetInstr(m_);
  auto all_instrs = AbsKnob::GetInstrTree(m_);

  // top-level instr
  for (auto& instr : top_instrs) {
    ExecInstr(instr, false);
  }

  // child instr
  fmt::format_to(buff, "while (1) {{\n"
                       "  int schedule_counter = 0;\n");
  std::set<InstrPtr> tops(top_instrs.begin(), top_instrs.end());
  for (auto& instr : all_instrs) {
    if (tops.find(instr) == tops.end()) {
      ExecInstr(instr, true);
    }
  }
  fmt::format_to(buff, "  if (schedule_counter == 0) {{\n"
                       "    break;\n"
                       "  }}\n"
                       "}}\n");

  // done
  EndFuncDef(kernel_func, buff);

  CommitSource("compute.cc", dir, buff);
  return true;
}

bool Ilator::GenerateGlobalHeader(const std::string& dir) {
  fmt::memory_buffer buff;

  fmt::format_to(buff,
                 "#include <fstream>\n"
                 "#include <systemc.h>\n"
#ifdef ILATOR_PRECISE_MEM
                 "#include <map>\n"
#else
                 "#include <unordered_map>\n"
#endif
                 "SC_MODULE({project}) {{\n"
                 "  std::ofstream instr_log;\n"
                 "  void LogInstrSequence(const std::string& instr_name);\n",
                 fmt::arg("project", GetProjectName()));

  // input
  for (auto& var : AbsKnob::GetInp(m_)) {
    fmt::format_to(buff,
                   "  sc_in<{var_type}> {var_name}_in;\n"
                   "  {var_type} {var_name};\n",
                   fmt::arg("var_type", GetCxxType(var)),
                   fmt::arg("var_name", GetCxxName(var)));
  }

  // state and global vars (e.g., CONCAT)
  for (auto& var : AbsKnob::GetSttTree(m_)) {
    fmt::format_to(buff, "  {var_type} {var_name};\n",
                   fmt::arg("var_type", GetCxxType(var)),
                   fmt::arg("var_name", GetCxxName(var)));
  }
  for (auto& var : global_vars_) {
    fmt::format_to(buff, "  {var_type} {var_name};\n",
                   fmt::arg("var_type", GetCxxType(var)),
                   fmt::arg("var_name", GetCxxName(var)));
  }

  // memory constant
  for (auto& mem : const_mems_) {
    fmt::format_to(buff, "  static {var_type} {var_name};\n",
                   fmt::arg("var_type", GetCxxType(mem)),
                   fmt::arg("var_name", GetCxxName(mem)));
  }

  // function declaration
  for (auto& func : functions_) {
    WriteFuncDecl(func.second, buff);
  }
  for (auto& func : externs_) {
    WriteFuncDecl(func.second, buff);
  }
  for (auto& func : memory_updates_) {
    WriteFuncDecl(func.second, buff);
  }

  // invoke
  fmt::format_to(buff,
                 "  SC_HAS_PROCESS({project});\n"
                 "  {project}(sc_module_name name_) : sc_module(name_) {{\n"
                 "    SC_METHOD(compute);\n"
                 "    sensitive",
                 fmt::arg("project", GetProjectName()));
  for (auto& var : AbsKnob::GetInp(m_)) {
    fmt::format_to(buff, " << {input_name}_in",
                   fmt::arg("input_name", GetCxxName(var)));
  }
  fmt::format_to(buff, ";\n"
                       "  }}\n");

  // done
  fmt::format_to(buff, "}};\n");

  // write to file
  auto file_path = os_portable_append_dir(dir, GetProjectName() + ".h");
  WriteFile(file_path, buff);
  return true;
}

bool Ilator::GenerateBuildSupport(const std::string& dir) {
  fmt::memory_buffer buff;

  // CMakeLists.txt
  std::vector<std::string> src_files;
  for (auto& f : source_files_) {
    src_files.push_back(
        fmt::format("  ${{CMAKE_CURRENT_SOURCE_DIR}}/{dir}/{file}",
                    fmt::arg("dir", dir_src), fmt::arg("file", f)));
  }
  fmt::format_to(
      buff,
      "# CMakeLists.txt for {project}\n"
      "cmake_minimum_required(VERSION 3.14.0)\n"
      "project({project} LANGUAGES CXX)\n"
      "\n"
      "option(ILATOR_VERBOSE \"Enable instruction sequence logging\" OFF)\n"
      "option(JSON_SUPPORT \"Build JSON parser support\" OFF)\n"
      "\n"
      "find_package(SystemCLanguage CONFIG REQUIRED)\n"
      "set(CMAKE_CXX_STANDARD ${{SystemC_CXX_STANDARD}})\n"
      "\n"
      "aux_source_directory(extern extern_src)\n"
      "add_executable({project}\n"
      "  ${{CMAKE_CURRENT_SOURCE_DIR}}/{dir_app}/main.cc\n"
      "  ${{extern_src}}\n"
      "{source_files}\n"
      ")\n"
      "\n"
      "target_include_directories({project} PRIVATE {dir_include})\n"
      "target_link_libraries({project} SystemC::systemc)\n"
      "if(${{ILATOR_VERBOSE}})\n"
      "  target_compile_definitions({project} PRIVATE ILATOR_VERBOSE)\n"
      "endif()\n"
      "if(${{JSON_SUPPORT}})\n"
      "  include(FetchContent)\n"
      "  FetchContent_Declare(\n"
      "    json\n"
      "    GIT_REPOSITORY https://github.com/nlohmann/json.git\n"
      "    GIT_TAG        v3.8.0\n"
      "  )\n"
      "  FetchContent_MakeAvailable(json)\n"
      "  target_link_libraries({project} nlohmann_json::nlohmann_json)\n"
      "endif()\n"
      //
      ,
      fmt::arg("project", GetProjectName()), fmt::arg("dir_app", dir_app),
      fmt::arg("source_files", fmt::join(src_files, "\n")),
      fmt::arg("dir_include", dir_include));
  WriteFile(os_portable_append_dir(dir, "CMakeLists.txt"), buff);

  // dummy main function if not exist
  auto app_template =
      os_portable_append_dir(os_portable_append_dir(dir, dir_app), "main.cc");
  if (!os_portable_exist(app_template)) {
    buff.clear();
    fmt::format_to(buff,
                   "#include <{project}.h>\n\n"
                   "int sc_main(int argc, char* argv[]) {{\n"
                   "  return 0; \n"
                   "}}\n",
                   fmt::arg("project", GetProjectName()));
    WriteFile(app_template, buff);
  }

  return true;
}

bool Ilator::RenderExpr(const ExprPtr& expr, StrBuff& buff, ExprVarMap& lut) {
  auto ExprDfsVisiter = [this, &buff, &lut](const ExprPtr& e) {
    if (auto pos = lut.find(e); pos == lut.end()) {
      if (e->is_var()) {
        DfsVar(e, buff, lut);
      } else if (e->is_const()) {
        DfsConst(e, buff, lut);
      } else {
        ILA_ASSERT(e->is_op());
        DfsOp(e, buff, lut);
      }
    }
    ILA_ASSERT((e->is_mem() && e->is_op()) || (lut.find(e) != lut.end()));
  };

  try {
    expr->DepthFirstVisit(ExprDfsVisiter);
  } catch (std::exception& err) {
    ILA_ERROR << err.what();
    return false;
  }
  return true;
}

Ilator::CxxFunc* Ilator::RegisterFunction(const std::string& func_name,
                                          ExprPtr return_expr) {
  auto func = new CxxFunc(func_name, return_expr);
  auto [it, status] = functions_.insert({func->name, func});
  ILA_ASSERT(status);
  return func;
}

Ilator::CxxFunc* Ilator::RegisterExternalFunc(const FuncPtr& func) {
  auto func_cxx = new CxxFunc(func->name().str(), func->out());
  auto [it, status] = externs_.insert({func_cxx->name, func_cxx});
  // uninterpreted function can have multiple occurrence
  if (status) {
    for (size_t i = 0; i < func->arg_num(); i++) {
      it->second->args.push_back(func->arg(i));
    }
  } else {
    delete func_cxx;
  }
  return it->second;
}

Ilator::CxxFunc* Ilator::RegisterMemoryUpdate(const ExprPtr& mem) {
  auto func_cxx = new CxxFunc(GetMemoryFuncName(mem), NULL, mem);
  auto [it, status] = memory_updates_.insert({func_cxx->name, func_cxx});
  // memory updates can have multiple occurrence
  if (!status) {
    delete func_cxx;
  }
  return it->second;
}

void Ilator::BeginFuncDef(Ilator::CxxFunc* func, StrBuff& buff) const {
  ILA_ASSERT(func->args.empty()); // no definition for uninterpreted funcs

  auto type = (func->ret) ? GetCxxType(func->ret) : GetCxxType(func->ret_type);
  auto args = (func->target)
                  ? fmt::format("{}& tmp_memory", GetCxxType(func->target))
                  : "";

  fmt::format_to(buff, "{return_type} {project}::{func_name}({argument}) {{\n",
                 fmt::arg("return_type", type),
                 fmt::arg("project", GetProjectName()),
                 fmt::arg("func_name", func->name), fmt::arg("argument", args));
}

void Ilator::EndFuncDef(Ilator::CxxFunc* func, StrBuff& buff) const {
  if (func->ret) {
    fmt::format_to(buff, "return {};\n", GetCxxName(func->ret));
  }
  fmt::format_to(buff, "}}\n");
}

void Ilator::WriteFuncDecl(Ilator::CxxFunc* func, StrBuff& buff) const {
  auto type = (func->ret) ? GetCxxType(func->ret) : GetCxxType(func->ret_type);
  auto args = (func->target)
                  ? fmt::format("{}& tmp_memory", GetCxxType(func->target))
                  : "";
  if (!func->args.empty()) { // uninterpreted func only
    ILA_NOT_NULL(func->ret_type);
    std::vector<std::string> arg_list;
    for (const auto& a : func->args) {
      arg_list.push_back(GetCxxType(a));
    }
    args = fmt::format("{}", fmt::join(arg_list, ", "));
  }

  fmt::format_to(buff, "  {return_type} {func_name}({argument});\n",
                 fmt::arg("return_type", type),
                 fmt::arg("func_name", func->name), fmt::arg("argument", args));
}

void Ilator::CommitSource(const std::string& file_name, const std::string& dir,
                          const StrBuff& buff) {
  auto file_path = os_portable_append_dir(dir, file_name);
  auto [it, ret] = source_files_.insert(file_name);
  ILA_ASSERT(ret) << "Duplicated source file name " << file_name;

  WriteFile(file_path, buff);
}

std::string Ilator::GetCxxType(const SortPtr& sort) {
  if (!sort) {
    return "void";
  } else if (sort->is_bool()) {
    return "bool";
  } else if (sort->is_bv()) {
    return fmt::format("sc_biguint<{}>", sort->bit_width());
  } else {
    ILA_ASSERT(sort->is_mem());
#ifdef ILATOR_PRECISE_MEM
    return fmt::format(
        "std::map<sc_biguint<{addr_width}>, sc_biguint<{data_width}>>",
        fmt::arg("addr_width", sort->addr_width()),
        fmt::arg("data_width", sort->data_width()));
#else
    return "std::unordered_map<int, int>";
#endif
  }
}

std::string Ilator::GetCxxName(const ExprPtr& expr) {
  if (expr->is_var()) {
    return fmt::format("{}_{}", expr->host()->name().str(), expr->name().str());
  } else {
    return fmt::format("univ_var_{}", GetPivotalId(expr->name().id()));
  }
}

std::string Ilator::GetDecodeFuncName(const InstrPtr& instr) {
  return fmt::format("decode_{host}_{instr}",
                     fmt::arg("host", instr->host()->name().str()),
                     fmt::arg("instr", instr->name().str()));
}

std::string Ilator::GetUpdateFuncName(const InstrPtr& instr) {
  return fmt::format("update_{host}_{instr}",
                     fmt::arg("host", instr->host()->name().str()),
                     fmt::arg("instr", instr->name().str()));
}

std::string Ilator::GetMemoryFuncName(const ExprPtr& expr) {
  ILA_ASSERT(expr->is_mem());
  if (auto uid = GetUidExprOp(expr); uid == AST_UID_EXPR_OP::ITE) {
    return fmt::format("ite_{}", GetPivotalId(expr->name().id()));
  } else {
    return fmt::format("store_{}", GetPivotalId(expr->name().id()));
  }
}

} // namespace ilang
