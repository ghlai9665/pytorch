#include <torch/csrc/jit/api/function_impl.h>
#include <torch/csrc/jit/passes/inliner.h>

#include <torch/csrc/jit/frontend/error_report.h>
#include <torch/csrc/jit/passes/constant_pooling.h>
#include <torch/csrc/jit/passes/constant_propagation.h>
#include <torch/csrc/jit/passes/peephole.h>

namespace torch {
namespace jit {
namespace {
c10::FunctionSchema defaultSchemaFor(const Function& function) {
  std::vector<c10::Argument> args;
  std::vector<c10::Argument> returns;
  Graph& g = *function.graph();
  size_t num_inputs = function.num_inputs();
  for (size_t i = 0; i < num_inputs; ++i) {
    const Value* v = g.inputs().at(i);
    std::string name = v->hasDebugName() ? v->debugNameBase()
                                         : ("argument_" + c10::to_string(i));
    args.emplace_back(std::move(name), unshapedType(g.inputs()[i]->type()));
  }
  for (size_t i = 0; i < g.outputs().size(); ++i) {
    returns.emplace_back("", unshapedType(g.outputs()[i]->type()));
  }
  return {function.name(), "", std::move(args), std::move(returns)};
}
} // namespace

void placeholderCreator(GraphFunction&) {
  throw RecursiveMethodCallError();
}

void GraphFunction::run(Stack& stack) {
  get_executor().run(stack);
}

void GraphFunction::run(Stack&& stack) {
  run(stack);
}

c10::intrusive_ptr<c10::ivalue::Future> GraphFunction::runAsync(
    Stack& stack,
    TaskLauncher taskLauncher) {
  return get_executor().runAsync(stack, std::move(taskLauncher));
}

size_t GraphFunction::computeInputTypesHash(
    const std::vector<IValue>& stack) const {
  // Use an algorithm similar to boost::hash_combine to compute the vector hash
  size_t r = 0;
  const size_t magic_number = 0x9e3779b9;
  for (const IValue& iv : stack) {
    r ^= std::hash<uint32_t>{}(iv.tagAsInt()) + magic_number + (r << 6) +
        (r >> 2);
  }
  return r;
}

IValue GraphFunction::operator()(
    std::vector<IValue> stack,
    const Kwargs& kwargs) {
  bool need_schema_check = true;
  if (!kwargs.size()) { // Fast path
    size_t input_types_hash = computeInputTypesHash(stack);
    if (!schema_checks_cache_.count(input_types_hash)) {
      getSchema().checkAndNormalizeInputs(stack, kwargs);
      schema_checks_cache_.insert(input_types_hash);
    }
    need_schema_check = false;
  }
  if (need_schema_check) {
    getSchema().checkAndNormalizeInputs(stack, kwargs);
  }
  run(stack);
  return stack.front();
}

void GraphFunction::ensure_defined() {
  if (function_creator_) {
    auto creator = function_creator_;
    function_creator_ = placeholderCreator;
    creator(*this);
    function_creator_ = nullptr;
  }
  check_single_output();
}

const c10::FunctionSchema& GraphFunction::getSchema() const {
  if (schema_ == nullptr) {
    schema_ = std::make_unique<c10::FunctionSchema>(defaultSchemaFor(*this));
  }
  return *schema_;
}

void preoptimizeGraph(std::shared_ptr<Graph>& graph) {
  Inline(*graph);
  // Peephole Optimize cleans up many "is None" checks and creates constant prop
  // opportunities
  PeepholeOptimize(graph, true);
  // // AliasDb construction can be slow, so run it just on immutable types
  // // to clean up constant Ifs & other easy wins
  ConstantPropagationImmutableTypes(graph);
  ConstantPooling(graph);
}

} // namespace jit
} // namespace torch
