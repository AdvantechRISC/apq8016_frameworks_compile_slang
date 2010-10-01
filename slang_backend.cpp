#include "slang.hpp"
#include "slang_backend.hpp"

#include "llvm/Module.h"
#include "llvm/Metadata.h"
#include "llvm/LLVMContext.h"

#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Target/TargetRegistry.h"
#include "llvm/Target/SubtargetFeature.h"

#include "llvm/CodeGen/RegAllocRegistry.h"
#include "llvm/CodeGen/SchedulerRegistry.h"

#include "llvm/Assembly/PrintModulePass.h"
#include "llvm/Bitcode/ReaderWriter.h"

#include "clang/AST/Decl.h"
#include "clang/AST/DeclGroup.h"
#include "clang/AST/ASTContext.h"

#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/TargetOptions.h"

#include "clang/Frontend/FrontendDiagnostic.h"

#include "clang/CodeGen/ModuleBuilder.h"

using namespace slang;

bool Backend::CreateCodeGenPasses() {
  if (mOutputType != SlangCompilerOutput_Assembly &&
      mOutputType != SlangCompilerOutput_Obj)
    return true;

  // Now we add passes for code emitting
  if (mCodeGenPasses) {
    return true;
  } else {
    mCodeGenPasses = new llvm::FunctionPassManager(mpModule);
    mCodeGenPasses->add(new llvm::TargetData(*mpTargetData));
  }

  // Create the TargetMachine for generating code.
  std::string Triple = mpModule->getTargetTriple();

  std::string Error;
  const llvm::Target* TargetInfo =
      llvm::TargetRegistry::lookupTarget(Triple, Error);
  if(TargetInfo == NULL) {
    mDiags.Report(clang::diag::err_fe_unable_to_create_target) << Error;
    return false;
  }

  llvm::NoFramePointerElim = mCodeGenOpts.DisableFPElim;

  // Use hardware FPU.
  //
  // FIXME: Need to detect the CPU capability and decide whether to use softfp.
  // To use softfp, change following 2 lines to
  //
  //  llvm::FloatABIType = llvm::FloatABI::Soft;
  //  llvm::UseSoftFloat = true;
  llvm::FloatABIType = llvm::FloatABI::Hard;
  llvm::UseSoftFloat = false;

  // BCC needs all unknown symbols resolved at compilation time. So we don't
  // need any relocation model.
  llvm::TargetMachine::setRelocationModel(llvm::Reloc::Static);


  // The target with pointer size greater than 32 (e.g. x86_64 architecture) may
  // need large data address model
  if (mpTargetData->getPointerSizeInBits() > 32)
    llvm::TargetMachine::setCodeModel(llvm::CodeModel::Medium);
  else
    // This is set for the linker (specify how large of the virtual addresses we
    // can access for all unknown symbols.)

    llvm::TargetMachine::setCodeModel(llvm::CodeModel::Small);

  // Setup feature string
  std::string FeaturesStr;
  if (mTargetOpts.CPU.size() || mTargetOpts.Features.size()) {
    llvm::SubtargetFeatures Features;

    Features.setCPU(mTargetOpts.CPU);

    for (std::vector<std::string>::const_iterator
             I = mTargetOpts.Features.begin(), E = mTargetOpts.Features.end();
         I != E;
         I++)
      Features.AddFeature(*I);

    FeaturesStr = Features.getString();
  }
  llvm::TargetMachine *TM =
      TargetInfo->createTargetMachine(Triple, FeaturesStr);

  // Register scheduler
  llvm::RegisterScheduler::setDefault(llvm::createDefaultScheduler);

  // Register allocation policy:
  //  createFastRegisterAllocator: fast but bad quality
  //  createLinearScanRegisterAllocator: not so fast but good quality
  llvm::RegisterRegAlloc::setDefault((mCodeGenOpts.OptimizationLevel == 0) ?
                                     llvm::createFastRegisterAllocator :
                                     llvm::createLinearScanRegisterAllocator);

  llvm::CodeGenOpt::Level OptLevel = llvm::CodeGenOpt::Default;
  if (mCodeGenOpts.OptimizationLevel == 0)
    OptLevel = llvm::CodeGenOpt::None;
  else if (mCodeGenOpts.OptimizationLevel == 3)
    OptLevel = llvm::CodeGenOpt::Aggressive;

  llvm::TargetMachine::CodeGenFileType CGFT =
      llvm::TargetMachine::CGFT_AssemblyFile;;
  if (mOutputType == SlangCompilerOutput_Obj)
    CGFT = llvm::TargetMachine::CGFT_ObjectFile;
  if (TM->addPassesToEmitFile(*mCodeGenPasses, FormattedOutStream,
                              CGFT, OptLevel)) {
    mDiags.Report(clang::diag::err_fe_unable_to_interface_with_target);
    return false;
  }

  return true;
}

Backend::Backend(clang::Diagnostic &Diags,
                 const clang::CodeGenOptions &CodeGenOpts,
                 const clang::TargetOptions &TargetOpts,
                 const PragmaList &Pragmas,
                 llvm::raw_ostream *OS,
                 SlangCompilerOutputTy OutputType,
                 clang::SourceManager &SourceMgr,
                 bool AllowRSPrefix) :
    ASTConsumer(),
    mCodeGenOpts(CodeGenOpts),
    mTargetOpts(TargetOpts),
    mSourceMgr(SourceMgr),
    mpOS(OS),
    mOutputType(OutputType),
    mpTargetData(NULL),
    mGen(NULL),
    mPerFunctionPasses(NULL),
    mPerModulePasses(NULL),
    mCodeGenPasses(NULL),
    mAllowRSPrefix(AllowRSPrefix),
    mLLVMContext(llvm::getGlobalContext()),
    mDiags(Diags),
    mpModule(NULL),
    mPragmas(Pragmas)
{
  FormattedOutStream.setStream(*mpOS, llvm::formatted_raw_ostream::PRESERVE_STREAM);
  mGen = CreateLLVMCodeGen(mDiags, "", mCodeGenOpts, mLLVMContext);
  return;
}

void Backend::Initialize(clang::ASTContext &Ctx) {
  mGen->Initialize(Ctx);

  mpModule = mGen->GetModule();
  mpTargetData = new llvm::TargetData(Slang::TargetDescription);

  return;
}

void Backend::HandleTopLevelDecl(clang::DeclGroupRef D) {
  // Disallow user-defined functions with prefix "rs"
  if (!mAllowRSPrefix) {
    clang::DeclGroupRef::iterator I;
    for (I = D.begin(); I != D.end(); I++) {
      clang::FunctionDecl *FD = dyn_cast<clang::FunctionDecl>(*I);
      if (!FD || !FD->isThisDeclarationADefinition()) continue;
      if (FD->getName().startswith("rs")) {
        mDiags.Report(clang::FullSourceLoc(FD->getLocStart(), mSourceMgr),
                      mDiags.getCustomDiagID(clang::Diagnostic::Error,
                                             "invalid function name prefix,"
                                             " \"rs\" is reserved: '%0'")
                      )
            << FD->getNameAsString();
      }
    }
  }

  mGen->HandleTopLevelDecl(D);
  return;
}

void Backend::HandleTranslationUnit(clang::ASTContext &Ctx) {
  mGen->HandleTranslationUnit(Ctx);

  // Here, we complete a translation unit (whole translation unit is now in LLVM
  // IR). Now, interact with LLVM backend to generate actual machine code (asm
  // or machine code, whatever.)

  // Silently ignore if we weren't initialized for some reason.
  if (!mpModule || !mpTargetData)
    return;

  llvm::Module *M = mGen->ReleaseModule();
  if (!M) {
    // The module has been released by IR gen on failures, do not double free.
    mpModule = NULL;
    return;
  }

  assert(mpModule == M && "Unexpected module change during LLVM IR generation");

  // Insert #pragma information into metadata section of module
  if (!mPragmas.empty()) {
    llvm::NamedMDNode *PragmaMetadata =
        mpModule->getOrInsertNamedMetadata(Slang::PragmaMetadataName);
    for (PragmaList::const_iterator I = mPragmas.begin(), E = mPragmas.end();
         I != E;
         I++) {
      llvm::SmallVector<llvm::Value*, 2> Pragma;
      // Name goes first
      Pragma.push_back(llvm::MDString::get(mLLVMContext, I->first));
      // And then value
      Pragma.push_back(llvm::MDString::get(mLLVMContext, I->second));
      // Create MDNode and insert into PragmaMetadata
      PragmaMetadata->addOperand(
          llvm::MDNode::get(mLLVMContext, Pragma.data(), Pragma.size()));
    }
  }

  HandleTranslationUnitEx(Ctx);

  // Create passes for optimization and code emission

  // Create and run per-function passes
  CreateFunctionPasses();
  if (mPerFunctionPasses) {
    mPerFunctionPasses->doInitialization();

    for (llvm::Module::iterator I = mpModule->begin(), E = mpModule->end();
         I != E;
         I++)
      if (!I->isDeclaration())
        mPerFunctionPasses->run(*I);

    mPerFunctionPasses->doFinalization();
  }

  // Create and run module passes
  CreateModulePasses();
  if (mPerModulePasses)
    mPerModulePasses->run(*mpModule);

  switch (mOutputType) {
    case SlangCompilerOutput_Assembly:
    case SlangCompilerOutput_Obj: {
      if(!CreateCodeGenPasses())
        return;

      mCodeGenPasses->doInitialization();

      for (llvm::Module::iterator I = mpModule->begin(), E = mpModule->end();
          I != E;
          I++)
        if(!I->isDeclaration())
          mCodeGenPasses->run(*I);

      mCodeGenPasses->doFinalization();
      break;
    }
    case SlangCompilerOutput_LL: {
      llvm::PassManager *LLEmitPM = new llvm::PassManager();
      LLEmitPM->add(llvm::createPrintModulePass(&FormattedOutStream));
      LLEmitPM->run(*mpModule);
      break;
    }

    case SlangCompilerOutput_Bitcode: {
      llvm::PassManager *BCEmitPM = new llvm::PassManager();
      BCEmitPM->add(llvm::createBitcodeWriterPass(FormattedOutStream));
      BCEmitPM->run(*mpModule);
      break;
    }
    case SlangCompilerOutput_Nothing: {
      return;
      break;
    }
    default: {
      assert(false && "Unknown output type");
      break;
    }
  }

  FormattedOutStream.flush();

  return;
}

void Backend::HandleTagDeclDefinition(clang::TagDecl *D) {
  mGen->HandleTagDeclDefinition(D);
  return;
}

void Backend::CompleteTentativeDefinition(clang::VarDecl *D) {
  mGen->CompleteTentativeDefinition(D);
  return;
}

Backend::~Backend() {
  delete mpModule;
  delete mpTargetData;
  delete mGen;
  delete mPerFunctionPasses;
  delete mPerModulePasses;
  delete mCodeGenPasses;
  return;
}
