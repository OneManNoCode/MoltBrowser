// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// ggml_vk_glslc: the subset of glslc (shaderc's command-line compiler) that
// llama.cpp's vulkan-shaders-gen runs for every shader variant, built on
// Chromium's own glslang and SPIRV-Tools. Chromium has no shaderc, and
// ggml-vulkan's SPIR-V is generated at build time rather than vendored
// (docs/design/SANDBOXED_INFERENCE.md section 11.2). Host tool only.
//
//   ggml_vk_glslc -fshader-stage=compute --target-env=vulkan1.2|vulkan1.3
//                 INPUT -o OUTPUT [-O] [-g] [-MD -MF DEPFILE]
//                 [-DNAME[=VALUE]]...
//
// As glslc does:
//  - #include "x" resolves next to the including file
//    (GL_GOOGLE_include_directive is enabled for every shader).
//  - -DNAME=VALUE becomes "#define NAME VALUE" in the preamble.
//  - vulkan1.2 targets SPIR-V 1.5, vulkan1.3 SPIR-V 1.6.
//  - -O runs SPIRV-Tools' performance passes (spirv-opt -O); without it
//    the SPIR-V is not optimized. -g keeps debug information.
//  - -MD -MF writes a Makefile depfile: OUTPUT: INPUT INCLUDES...
//  - Diagnostics, warnings included, go to stderr; vulkan-shaders-gen
//    treats any stderr output as a failure. Exit status 0 on success.
// Anything else on the command line is an error.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "SPIRV/GlslangToSpv.h"
#include "glslang/Public/ResourceLimits.h"
#include "glslang/Public/ShaderLang.h"
#include "spirv-tools/optimizer.hpp"

namespace {

struct Options {
  std::string input;
  std::string output;
  std::string depfile;
  bool vulkan13 = false;
  bool optimize = false;
  bool debug_info = false;
  bool make_depfile = false;
  // In command-line order; a later definition of the same name wins, as
  // with glslc.
  std::vector<std::pair<std::string, std::string>> defines;
};

int Fail(const std::string& message) {
  std::fprintf(stderr, "ggml_vk_glslc: %s\n", message.c_str());
  return 1;
}

// Strips one pair of surrounding double quotes. vulkan-shaders-gen quotes
// paths on Windows; the C runtime normally removes them already.
std::string Unquote(const std::string& s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

bool ParseArgs(int argc, char** argv, Options* options, std::string* error) {
  bool stage_seen = false;
  bool env_seen = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-fshader-stage=compute") {
      stage_seen = true;
    } else if (arg == "--target-env=vulkan1.2") {
      env_seen = true;
      options->vulkan13 = false;
    } else if (arg == "--target-env=vulkan1.3") {
      env_seen = true;
      options->vulkan13 = true;
    } else if (arg == "-o") {
      if (++i >= argc) {
        *error = "-o needs a file name";
        return false;
      }
      options->output = Unquote(argv[i]);
    } else if (arg == "-O") {
      options->optimize = true;
    } else if (arg == "-g") {
      options->debug_info = true;
    } else if (arg == "-MD") {
      options->make_depfile = true;
    } else if (arg == "-MF") {
      if (++i >= argc) {
        *error = "-MF needs a file name";
        return false;
      }
      options->depfile = Unquote(argv[i]);
    } else if (arg.rfind("-D", 0) == 0 && arg.size() > 2) {
      const std::string def = arg.substr(2);
      const size_t eq = def.find('=');
      std::string name = eq == std::string::npos ? def : def.substr(0, eq);
      std::string value =
          eq == std::string::npos ? std::string() : def.substr(eq + 1);
      if (name.empty()) {
        *error = "empty macro name in " + arg;
        return false;
      }
      options->defines.emplace_back(std::move(name), std::move(value));
    } else if (!arg.empty() && arg[0] == '-') {
      *error = "unsupported option " + arg;
      return false;
    } else {
      if (!options->input.empty()) {
        *error = "more than one input file";
        return false;
      }
      options->input = Unquote(arg);
    }
  }
  if (!stage_seen) {
    *error = "-fshader-stage=compute is required";
  } else if (!env_seen) {
    *error = "--target-env=vulkan1.2 or vulkan1.3 is required";
  } else if (options->input.empty()) {
    *error = "no input file";
  } else if (options->output.empty()) {
    *error = "no output file (-o)";
  } else if (options->make_depfile != !options->depfile.empty()) {
    *error = "-MD and -MF go together";
  }
  return error->empty();
}

bool ReadFile(const std::string& path, std::string* contents) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  if (!in.good() && !in.eof()) return false;
  *contents = ss.str();
  return true;
}

std::string DirName(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string JoinPath(const std::string& dir, const std::string& name) {
  if (dir.empty()) return name;
  return dir + "/" + name;
}

// #include "x" next to the including file, as glslc's file includer does.
// <x> (system includes) would need -I directories; glslc has none here.
class Includer : public glslang::TShader::Includer {
 public:
  IncludeResult* includeLocal(const char* header_name,
                              const char* includer_name,
                              size_t /*inclusion_depth*/) override {
    const std::string path =
        JoinPath(DirName(includer_name ? includer_name : ""), header_name);
    auto contents = std::make_unique<std::string>();
    if (!ReadFile(path, contents.get())) return nullptr;
    if (seen_.insert(path).second) included_.push_back(path);
    std::string* data = contents.release();
    return new IncludeResult(path, data->data(), data->size(), data);
  }

  IncludeResult* includeSystem(const char*, const char*, size_t) override {
    return nullptr;
  }

  void releaseInclude(IncludeResult* result) override {
    if (!result) return;
    delete static_cast<std::string*>(result->userData);
    delete result;
  }

  const std::vector<std::string>& included() const { return included_; }

 private:
  std::vector<std::string> included_;
  std::set<std::string> seen_;
};

std::string EscapeDepPath(const std::string& path) {
  std::string out;
  for (char c : path) {
    if (c == ' ' || c == '#') out += '\\';
    if (c == '$') out += '$';
    out += (c == '\\') ? '/' : c;
  }
  return out;
}

bool WriteBinary(const std::string& path, const std::vector<uint32_t>& words) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(reinterpret_cast<const char*>(words.data()),
            static_cast<std::streamsize>(words.size() * sizeof(uint32_t)));
  return static_cast<bool>(out);
}

bool WriteText(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out << text;
  return static_cast<bool>(out);
}

// Prints a non-empty glslang log (errors and warnings) to stderr.
void PrintLog(const char* log) {
  if (log && *log) std::fputs(log, stderr);
}

int Compile(const Options& options) {
  std::string source;
  if (!ReadFile(options.input, &source)) {
    return Fail("cannot read " + options.input);
  }

  // glslc enables #include for every shader and turns -D into #define in
  // the preamble (which glslang reads after the #version line).
  std::string preamble = "#extension GL_GOOGLE_include_directive : require\n";
  std::map<std::string, std::string> last_definition;
  for (const auto& [name, value] : options.defines) {
    last_definition[name] = value;
  }
  for (const auto& [name, value] : options.defines) {
    auto it = last_definition.find(name);
    if (it == last_definition.end()) continue;  // already written
    preamble += "#define " + name;
    if (!it->second.empty()) preamble += " " + it->second;
    preamble += "\n";
    last_definition.erase(it);
  }

  const glslang::EShTargetClientVersion client =
      options.vulkan13 ? glslang::EShTargetVulkan_1_3
                       : glslang::EShTargetVulkan_1_2;
  const glslang::EShTargetLanguageVersion spirv_version =
      options.vulkan13 ? glslang::EShTargetSpv_1_6 : glslang::EShTargetSpv_1_5;
  const spv_target_env tools_env =
      options.vulkan13 ? SPV_ENV_VULKAN_1_3 : SPV_ENV_VULKAN_1_2;

  glslang::TShader shader(EShLangCompute);
  const char* strings[] = {source.c_str()};
  const int lengths[] = {static_cast<int>(source.size())};
  const char* names[] = {options.input.c_str()};
  shader.setStringsWithLengthsAndNames(strings, lengths, names, 1);
  shader.setPreamble(preamble.c_str());
  shader.setEntryPoint("main");
  shader.setSourceEntryPoint("main");
  shader.setEnvInput(glslang::EShSourceGlsl, EShLangCompute,
                     glslang::EShClientVulkan, 100);
  shader.setEnvClient(glslang::EShClientVulkan, client);
  shader.setEnvTarget(glslang::EShTargetSpv, spirv_version);

  const EShMessages messages =
      static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
  Includer includer;
  const bool parsed = shader.parse(GetDefaultResources(),
                                   /*defaultVersion=*/110,
                                   /*forwardCompatible=*/false, messages,
                                   includer);
  PrintLog(shader.getInfoLog());
  PrintLog(shader.getInfoDebugLog());
  if (!parsed) return 1;

  glslang::TProgram program;
  program.addShader(&shader);
  const bool linked = program.link(messages);
  PrintLog(program.getInfoLog());
  PrintLog(program.getInfoDebugLog());
  if (!linked) return 1;

  std::vector<uint32_t> spirv;
  spv::SpvBuildLogger logger;
  glslang::SpvOptions spv_options;
  spv_options.generateDebugInfo = options.debug_info;
  // Optimization is SPIRV-Tools' -O below, as in glslc; glslang's own pass
  // list differs.
  spv_options.disableOptimizer = true;
  spv_options.optimizeSize = false;
  spv_options.validate = false;
  glslang::GlslangToSpv(*program.getIntermediate(EShLangCompute), spirv,
                        &logger, &spv_options);
  const std::string spv_messages = logger.getAllMessages();
  if (!spv_messages.empty()) std::fputs(spv_messages.c_str(), stderr);
  if (spirv.empty()) return Fail("no SPIR-V generated for " + options.input);

  if (options.optimize) {
    spvtools::Optimizer optimizer(tools_env);
    bool optimizer_error = false;
    optimizer.SetMessageConsumer(
        [&optimizer_error](spv_message_level_t level, const char*,
                           const spv_position_t& position,
                           const char* message) {
          if (level > SPV_MSG_WARNING) return;
          optimizer_error |= level <= SPV_MSG_ERROR;
          std::fprintf(stderr, "spirv-opt: %zu: %s\n", position.index,
                       message ? message : "");
        });
    // As glslc: -O without -g strips debug information first.
    if (!options.debug_info) {
      optimizer.RegisterPass(spvtools::CreateStripDebugInfoPass());
    }
    optimizer.RegisterPerformancePasses();
    // The validator settings shaderc uses before optimizing.
    spvtools::ValidatorOptions validator_options;
    validator_options.SetSkipBlockLayout(true);
    validator_options.SetRelaxLogicalPointer(true);
    validator_options.SetBeforeHlslLegalization(true);
    spvtools::OptimizerOptions optimizer_options;
    optimizer_options.set_validator_options(validator_options);
    std::vector<uint32_t> optimized;
    if (!optimizer.Run(spirv.data(), spirv.size(), &optimized,
                       optimizer_options) ||
        optimizer_error) {
      return Fail("spirv-opt failed for " + options.input);
    }
    spirv.swap(optimized);
  }

  if (!WriteBinary(options.output, spirv)) {
    return Fail("cannot write " + options.output);
  }
  if (options.make_depfile) {
    std::string dep = EscapeDepPath(options.output) + ":";
    dep += " " + EscapeDepPath(options.input);
    for (const std::string& path : includer.included()) {
      dep += " " + EscapeDepPath(path);
    }
    dep += "\n";
    if (!WriteText(options.depfile, dep)) {
      return Fail("cannot write " + options.depfile);
    }
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  std::string error;
  if (!ParseArgs(argc, argv, &options, &error)) return Fail(error);
  if (!glslang::InitializeProcess()) return Fail("glslang failed to start");
  const int status = Compile(options);
  glslang::FinalizeProcess();
  return status;
}
