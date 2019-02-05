#ifndef REVNG_C_DECOMPILATIONPASS_H
#define REVNG_C_DECOMPILATIONPASS_H

#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>

struct DecompilationPass : public llvm::ModulePass {
  static char ID;

  DecompilationPass();
  DecompilationPass(const llvm::Function *Function,
                    std::unique_ptr<llvm::raw_ostream> Out);

  bool runOnModule(llvm::Module &F) override;

private:
  const llvm::Function *TheFunction;
  std::unique_ptr<llvm::raw_ostream> Out;
};

#endif /* ifndef REVNG_C_DECOMPILATIONPASS_H */