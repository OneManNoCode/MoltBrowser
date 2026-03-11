// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#ifndef MOLT_AI_PERSONAS_PERSONA_SYSTEM_H_
#define MOLT_AI_PERSONAS_PERSONA_SYSTEM_H_

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace molt_ai {

// Persona definition
struct Persona {
  std::string id;
  std::string name;
  std::string description;
  std::string focus;           // Primary domain focus
  std::string tone;            // Communication style
  std::string risk_tolerance;  // "conservative", "moderate", "aggressive"
  std::string domain_expertise;
  std::string system_prompt;   // Full system prompt for LLM
  bool is_builtin;             // Built-in vs user-created
  bool is_active;
};

// ============================================================
// PersonaSystem — AI persona management
// ============================================================
// Users can create profiles that modify AI reasoning:
//   Entrepreneur, Researcher, Developer, Lawyer, Investor
//
// Persona parameters:
//   - Focus area
//   - Communication tone
//   - Risk tolerance
//   - Domain expertise
//
// Example persona prompt injection:
//   SYSTEM: You are acting as persona: Lawyer
//   Focus on legal implications of the webpage.
//   Provide cautious interpretations.
// ============================================================
class PersonaSystem {
 public:
  PersonaSystem();
  ~PersonaSystem();

  // Initialize with storage path for custom personas
  bool Initialize(const std::string& storage_path);

  // ---- Persona Management ----

  // Get all available personas
  std::vector<Persona> GetAllPersonas() const;

  // Get a specific persona
  Persona GetPersona(const std::string& id) const;

  // Get the currently active persona
  Persona GetActivePersona() const;

  // Set the active persona
  bool SetActivePersona(const std::string& id);

  // Clear active persona (return to default)
  void ClearActivePersona();

  // ---- Custom Personas ----

  // Create a new custom persona
  std::string CreatePersona(const Persona& persona);

  // Update an existing custom persona
  bool UpdatePersona(const std::string& id, const Persona& persona);

  // Delete a custom persona
  bool DeletePersona(const std::string& id);

  // ---- Prompt Generation ----

  // Get the system prompt for the active persona
  std::string GetActiveSystemPrompt() const;

  // Generate a context-aware system prompt combining persona + page context
  std::string GenerateContextPrompt(const std::string& persona_id,
                                     const std::string& page_context) const;

 private:
  std::unordered_map<std::string, Persona> personas_;
  std::string active_persona_id_;
  std::string storage_path_;

  void InitializeBuiltinPersonas();
  void LoadCustomPersonas();
  void SaveCustomPersonas() const;
};

}  // namespace molt_ai

#endif  // MOLT_AI_PERSONAS_PERSONA_SYSTEM_H_
