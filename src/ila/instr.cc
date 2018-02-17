/// \file
/// The source for the class Instr.

#include "ila/instr.h"

namespace ila {

typedef Instr::InstrLvlAbsPtr InstrLvlAbsPtr;

Instr::Instr(const std::string& name, const InstrLvlAbsPtr host) : host_(host) {
  // update name if specified
  if (name != "")
    set_name(name);
  // initialization for other components
  updates_.clear();
}

Instr::~Instr(){};

InstrPtr Instr::New(const std::string& name, InstrLvlAbsPtr host) {
  return std::make_shared<Instr>(name, host);
}

bool Instr::has_view() const { return has_view_; }

InstrLvlAbsPtr Instr::host() const { return host_; }

void Instr::set_view(bool v) { has_view_ = v; }

void Instr::set_mngr(const ExprMngrPtr mngr) { expr_mngr_ = mngr; }

void Instr::SetDecode(const ExprPtr decode) {
  ILA_ERROR_IF(decode_)
      << "Decode for " << name()
      << "has been assigned. Use ForceSetDecode to overwrite.\n";

  if (!decode_) {
    ForceSetDecode(decode);
  }
}

void Instr::ForceSetDecode(const ExprPtr decode) {
  ILA_NOT_NULL(decode); // setting NULL pointer to decode function
  ILA_CHECK(decode->is_bool()) << "Decode must have Boolean sort.\n";

  decode_ = Unify(decode);
}

void Instr::AddUpdate(const std::string& name, const ExprPtr update) {
  auto pos = updates_.find(name);

  ILA_ERROR_IF(pos != updates_.end())
      << "Update function for " << name
      << " has been assigned. Use ForceAddUpdate to overwrite the result.\n";

  if (pos == updates_.end()) {
    ForceAddUpdate(name, update);
  }
}

void Instr::AddUpdate(const ExprPtr state, const ExprPtr update) {
  std::string name = state->name().str();
  AddUpdate(name, update);
}

void Instr::ForceAddUpdate(const std::string& name, const ExprPtr update) {
  ExprPtr sim_update = Unify(update);
  updates_[name] = sim_update;
}

ExprPtr Instr::GetDecode() const { return decode_; }

ExprPtr Instr::GetUpdate(const std::string& name) const {
  auto pos = updates_.find(name);

  if (pos != updates_.end()) {
    return pos->second;
  } else {
    return NULL;
  }
}

ExprPtr Instr::GetUpdate(const ExprPtr state) const {
  std::string name = state->name().str();
  return GetUpdate(name);
}

std::ostream& Instr::Print(std::ostream& out) const {
  out << "Instr." << name();
  return out;
}

std::ostream& operator<<(std::ostream& out, InstrPtr i) {
  return i->Print(out);
}

ExprPtr Instr::Unify(const ExprPtr e) {
  return expr_mngr_ ? expr_mngr_->GetRep(e) : e;
}

} // namespace ila

