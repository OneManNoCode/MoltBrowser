// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#ifndef MOLT_AI_DOM_DOM_INTERPRETER_H_
#define MOLT_AI_DOM_DOM_INTERPRETER_H_

#include <string>
#include <vector>

namespace molt_ai {

// Structured representation of a form field
struct FormField {
  std::string name;
  std::string type;       // "text", "email", "password", "select", etc.
  std::string label;
  std::string value;
  std::string placeholder;
  bool is_required;
  std::vector<std::string> options;  // For select/radio fields
};

// Structured representation of a form
struct FormInfo {
  std::string action;
  std::string method;
  std::string id;
  std::vector<FormField> fields;
};

// Structured representation of a link
struct LinkInfo {
  std::string text;
  std::string href;
  std::string aria_label;
  bool is_navigation;
};

// Structured representation of a button
struct ButtonInfo {
  std::string text;
  std::string type;
  std::string id;
  std::string aria_label;
  bool is_submit;
};

// Structured representation of a table
struct TableInfo {
  std::vector<std::string> headers;
  std::vector<std::vector<std::string>> rows;
};

// Structured representation of an image
struct ImageInfo {
  std::string src;
  std::string alt;
  int width;
  int height;
};

// ============================================================
// StructuredPage — Complete structured representation of a web page
// ============================================================
// This is the output of DOMInterpreter. It converts raw HTML DOM
// into a structured format that can be serialized to JSON and
// fed into an LLM's context window.
//
// Max output: 4000 tokens for LLM context window (from V4 spec)
// ============================================================
struct StructuredPage {
  std::string page_title;
  std::string page_url;
  std::string main_content;
  std::string meta_description;
  std::string language;

  std::vector<std::string> headings;
  std::vector<LinkInfo> links;
  std::vector<ButtonInfo> buttons;
  std::vector<FormInfo> forms;
  std::vector<TableInfo> tables;
  std::vector<ImageInfo> images;

  // Metadata
  int total_text_length;
  int estimated_tokens;
  bool was_truncated;

  // Serialize to JSON string for LLM context
  std::string ToJSON() const;

  // Serialize to compact text format for smaller context
  std::string ToCompactText() const;
};

// ============================================================
// DOMInterpreter — Web page understanding engine
// ============================================================
// Pipeline:
//   HTML DOM → Structured DOM → LLM Context → AI reasoning
//
// This module converts raw HTML DOM into structured data that
// enables AI to reason about:
//   - Page structure and layout
//   - Navigation options
//   - Interactive elements (forms, buttons)
//   - Content hierarchy
//   - Data tables
// ============================================================
class DOMInterpreter {
 public:
  DOMInterpreter();
  ~DOMInterpreter();

  // Parse raw HTML into structured page representation
  StructuredPage ParseHTML(const std::string& html,
                           const std::string& url) const;

  // Parse DOM from a live browser tab (integrates with Chromium content layer)
  StructuredPage ParseTab(int tab_id) const;

  // Extract only interactive elements (for agent actions)
  std::vector<ButtonInfo> ExtractButtons(const std::string& html) const;
  std::vector<LinkInfo> ExtractLinks(const std::string& html) const;
  std::vector<FormInfo> ExtractForms(const std::string& html) const;

  // Extract main content text (article body, removing nav/ads/footer)
  std::string ExtractMainContent(const std::string& html) const;

  // Extract structured data from specific CSS selectors
  std::string ExtractBySelector(const std::string& html,
                                 const std::string& selector) const;

  // Get element at coordinates (for agent click targeting)
  std::string GetElementAtPosition(int tab_id, int x, int y) const;

  // Truncate content to fit within token limit
  static std::string TruncateToTokenLimit(const std::string& content,
                                           int max_tokens);

 private:
  static constexpr int kMaxOutputTokens = 4000;

  // Internal parsing helpers
  std::string StripHTMLTags(const std::string& html) const;
  std::string CleanWhitespace(const std::string& text) const;
  std::vector<std::string> ExtractByTag(const std::string& html,
                                         const std::string& tag) const;
};

}  // namespace molt_ai

#endif  // MOLT_AI_DOM_DOM_INTERPRETER_H_
