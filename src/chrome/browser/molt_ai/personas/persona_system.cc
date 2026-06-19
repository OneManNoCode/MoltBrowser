// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/personas/persona_system.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/values.h"

namespace molt_ai {

PersonaSystem::PersonaSystem() = default;
PersonaSystem::~PersonaSystem() = default;

bool PersonaSystem::Initialize(const std::string& storage_path) {
  storage_path_ = storage_path;
  InitializeBuiltinPersonas();
  LoadCustomPersonas();
  return true;
}

void PersonaSystem::InitializeBuiltinPersonas() {
  // Researcher persona
  {
    Persona p;
    p.id = "researcher";
    p.name = "Researcher";
    p.description = "Academic and scientific research focus";
    p.focus = "Research, analysis, citations, data interpretation";
    p.tone = "Analytical and thorough";
    p.risk_tolerance = "moderate";
    p.domain_expertise = "Academic research, scientific methodology";
    p.system_prompt =
        "You are acting as persona: Researcher.\n"
        "Focus on factual accuracy and evidence-based analysis.\n"
        "Cite sources when possible. Identify methodological strengths "
        "and weaknesses. Provide balanced interpretations of data.\n"
        "Prioritize peer-reviewed sources and primary data.";
    p.is_builtin = true;
    p.is_active = false;
    personas_[p.id] = p;
  }

  // Developer persona
  {
    Persona p;
    p.id = "developer";
    p.name = "Developer";
    p.description = "Software engineering and technical focus";
    p.focus = "Code quality, architecture, debugging, documentation";
    p.tone = "Technical and precise";
    p.risk_tolerance = "moderate";
    p.domain_expertise = "Software engineering, systems design";
    p.system_prompt =
        "You are acting as persona: Developer.\n"
        "Focus on code quality, architecture patterns, and technical "
        "implementation details. Analyze pages for API documentation, "
        "code examples, and technical specifications.\n"
        "Identify potential bugs, security issues, and optimization "
        "opportunities. Prefer practical solutions over theoretical.";
    p.is_builtin = true;
    p.is_active = false;
    personas_[p.id] = p;
  }

  // Lawyer persona
  {
    Persona p;
    p.id = "lawyer";
    p.name = "Lawyer";
    p.description = "Legal analysis and compliance focus";
    p.focus = "Legal implications, terms of service, compliance, risk";
    p.tone = "Cautious and precise";
    p.risk_tolerance = "conservative";
    p.domain_expertise = "Legal analysis, contract review, compliance";
    p.system_prompt =
        "You are acting as persona: Lawyer.\n"
        "Focus on legal implications of the webpage content.\n"
        "Provide cautious interpretations. Identify potential legal "
        "risks, terms of service implications, and compliance issues.\n"
        "Flag privacy concerns and data collection practices.\n"
        "Note: This is not legal advice. Recommend consulting a "
        "qualified attorney for specific legal matters.";
    p.is_builtin = true;
    p.is_active = false;
    personas_[p.id] = p;
  }

  // Entrepreneur persona
  {
    Persona p;
    p.id = "entrepreneur";
    p.name = "Entrepreneur";
    p.description = "Business strategy and opportunity focus";
    p.focus = "Market opportunities, business models, competitive analysis";
    p.tone = "Strategic and action-oriented";
    p.risk_tolerance = "aggressive";
    p.domain_expertise = "Business strategy, market analysis, startups";
    p.system_prompt =
        "You are acting as persona: Entrepreneur.\n"
        "Focus on business opportunities, market signals, and "
        "competitive positioning. Analyze pages for market data, "
        "competitor intelligence, and growth opportunities.\n"
        "Think in terms of addressable market, business models, and "
        "strategic advantage. Be action-oriented and decisive.";
    p.is_builtin = true;
    p.is_active = false;
    personas_[p.id] = p;
  }

  // Investor persona
  {
    Persona p;
    p.id = "investor";
    p.name = "Investor";
    p.description = "Financial analysis and investment focus";
    p.focus = "Financial data, valuations, market trends, risk assessment";
    p.tone = "Analytical and risk-aware";
    p.risk_tolerance = "moderate";
    p.domain_expertise = "Financial analysis, investment research";
    p.system_prompt =
        "You are acting as persona: Investor.\n"
        "Focus on financial data, market trends, and investment "
        "implications. Analyze pages for financial metrics, "
        "growth indicators, and risk factors.\n"
        "Consider both upside potential and downside risks.\n"
        "Note: This is not financial advice. Always conduct your "
        "own due diligence.";
    p.is_builtin = true;
    p.is_active = false;
    personas_[p.id] = p;
  }

  // Security Analyst persona
  {
    Persona p;
    p.id = "security_analyst";
    p.name = "Security Analyst";
    p.description = "Cybersecurity and threat analysis focus";
    p.focus = "Security vulnerabilities, threats, privacy, data protection";
    p.tone = "Vigilant and methodical";
    p.risk_tolerance = "conservative";
    p.domain_expertise = "Cybersecurity, threat analysis, privacy";
    p.system_prompt =
        "You are acting as persona: Security Analyst.\n"
        "Focus on security implications of web content.\n"
        "Identify potential vulnerabilities, suspicious elements, "
        "tracking mechanisms, and privacy concerns.\n"
        "Analyze SSL certificates, data collection practices, and "
        "third-party scripts. Flag any security red flags.";
    p.is_builtin = true;
    p.is_active = false;
    personas_[p.id] = p;
  }
}

void PersonaSystem::LoadCustomPersonas() {
  if (storage_path_.empty())
    return;

  base::FilePath json_path =
      base::FilePath::FromUTF8Unsafe(storage_path_).Append(FILE_PATH_LITERAL("personas.json"));

  std::string json_content;
  if (!base::ReadFileToString(json_path, &json_content))
    return;

  auto parsed = base::JSONReader::Read(json_content,
                                        base::JSON_ALLOW_TRAILING_COMMAS);
  if (!parsed || !parsed->is_list()) {
    LOG(WARNING) << "[MoltAI] Invalid personas.json format";
    return;
  }

  for (const auto& item : parsed->GetList()) {
    if (!item.is_dict())
      continue;
    const auto& dict = item.GetDict();

    Persona p;
    if (auto* v = dict.FindString("id")) p.id = *v;
    if (auto* v = dict.FindString("name")) p.name = *v;
    if (auto* v = dict.FindString("description")) p.description = *v;
    if (auto* v = dict.FindString("focus")) p.focus = *v;
    if (auto* v = dict.FindString("tone")) p.tone = *v;
    if (auto* v = dict.FindString("risk_tolerance")) p.risk_tolerance = *v;
    if (auto* v = dict.FindString("domain_expertise")) p.domain_expertise = *v;
    if (auto* v = dict.FindString("system_prompt")) p.system_prompt = *v;
    p.is_builtin = false;
    p.is_active = false;

    if (!p.id.empty() && !p.name.empty()) {
      personas_[p.id] = p;
      LOG(INFO) << "[MoltAI] Loaded custom persona: " << p.name;
    }
  }
}

void PersonaSystem::SaveCustomPersonas() const {
  if (storage_path_.empty())
    return;

  base::Value::List personas_list;
  for (const auto& [id, persona] : personas_) {
    if (persona.is_builtin)
      continue;

    base::Value::Dict dict;
    dict.Set("id", persona.id);
    dict.Set("name", persona.name);
    dict.Set("description", persona.description);
    dict.Set("focus", persona.focus);
    dict.Set("tone", persona.tone);
    dict.Set("risk_tolerance", persona.risk_tolerance);
    dict.Set("domain_expertise", persona.domain_expertise);
    dict.Set("system_prompt", persona.system_prompt);
    personas_list.Append(std::move(dict));
  }

  std::string json_output;
  base::JSONWriter::WriteWithOptions(
      base::Value(std::move(personas_list)),
      base::JSONWriter::OPTIONS_PRETTY_PRINT, &json_output);

  base::FilePath dir_path = base::FilePath::FromUTF8Unsafe(storage_path_);
  base::CreateDirectory(dir_path);

  base::FilePath json_path =
      dir_path.Append(FILE_PATH_LITERAL("personas.json"));
  base::WriteFile(json_path, json_output);

  LOG(INFO) << "[MoltAI] Saved custom personas to " << json_path.value();
}

std::vector<Persona> PersonaSystem::GetAllPersonas() const {
  std::vector<Persona> result;
  for (const auto& [id, persona] : personas_) {
    result.push_back(persona);
  }
  return result;
}

Persona PersonaSystem::GetPersona(const std::string& id) const {
  auto it = personas_.find(id);
  if (it != personas_.end()) {
    return it->second;
  }
  return {};
}

Persona PersonaSystem::GetActivePersona() const {
  if (active_persona_id_.empty()) {
    return {};
  }
  return GetPersona(active_persona_id_);
}

bool PersonaSystem::SetActivePersona(const std::string& id) {
  if (personas_.find(id) == personas_.end()) {
    return false;
  }

  if (!active_persona_id_.empty()) {
    personas_[active_persona_id_].is_active = false;
  }

  active_persona_id_ = id;
  personas_[id].is_active = true;
  return true;
}

void PersonaSystem::ClearActivePersona() {
  if (!active_persona_id_.empty()) {
    personas_[active_persona_id_].is_active = false;
    active_persona_id_.clear();
  }
}

std::string PersonaSystem::CreatePersona(const Persona& persona) {
  Persona p = persona;
  p.is_builtin = false;

  if (p.id.empty()) {
    p.id = p.name;
    std::transform(p.id.begin(), p.id.end(), p.id.begin(), ::tolower);
    std::replace(p.id.begin(), p.id.end(), ' ', '_');
  }

  // Generate system prompt if not provided
  if (p.system_prompt.empty()) {
    p.system_prompt =
        "You are acting as persona: " + p.name + ".\n" +
        (!p.focus.empty() ? "Focus on: " + p.focus + ".\n" : "") +
        (!p.tone.empty() ? "Communication style: " + p.tone + ".\n" : "") +
        (!p.domain_expertise.empty()
             ? "Domain expertise: " + p.domain_expertise + ".\n"
             : "") +
        (!p.description.empty() ? p.description + "\n" : "");
  }

  personas_[p.id] = p;
  SaveCustomPersonas();
  return p.id;
}

bool PersonaSystem::UpdatePersona(const std::string& id,
                                   const Persona& persona) {
  auto it = personas_.find(id);
  if (it == personas_.end() || it->second.is_builtin) {
    return false;
  }

  personas_[id] = persona;
  personas_[id].id = id;
  personas_[id].is_builtin = false;
  SaveCustomPersonas();
  return true;
}

bool PersonaSystem::DeletePersona(const std::string& id) {
  auto it = personas_.find(id);
  if (it == personas_.end() || it->second.is_builtin) {
    return false;
  }

  if (active_persona_id_ == id) {
    active_persona_id_.clear();
  }

  personas_.erase(it);
  SaveCustomPersonas();
  return true;
}

std::string PersonaSystem::GetActiveSystemPrompt() const {
  if (active_persona_id_.empty()) {
    return "";
  }
  auto persona = GetPersona(active_persona_id_);
  return persona.system_prompt;
}

std::string PersonaSystem::GenerateContextPrompt(
    const std::string& persona_id,
    const std::string& page_context) const {
  auto persona = GetPersona(persona_id);
  if (persona.id.empty()) {
    return page_context;
  }

  return persona.system_prompt + "\n\n"
         "Current page context:\n" + page_context;
}

}  // namespace molt_ai
