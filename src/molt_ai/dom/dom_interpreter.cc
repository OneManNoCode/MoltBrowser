// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "src/molt_ai/dom/dom_interpreter.h"

#include <algorithm>
#include <regex>
#include <sstream>

namespace molt_ai {

DOMInterpreter::DOMInterpreter() = default;
DOMInterpreter::~DOMInterpreter() = default;

StructuredPage DOMInterpreter::ParseHTML(const std::string& html,
                                          const std::string& url) const {
  StructuredPage page;
  page.page_url = url;
  page.was_truncated = false;

  // Extract title
  std::regex title_re("<title[^>]*>(.*?)</title>", std::regex::icase);
  std::smatch title_match;
  if (std::regex_search(html, title_match, title_re)) {
    page.page_title = title_match[1].str();
  }

  // Extract meta description
  std::regex meta_re(
      R"(<meta\s+name=["']description["']\s+content=["'](.*?)["'])",
      std::regex::icase);
  std::smatch meta_match;
  if (std::regex_search(html, meta_match, meta_re)) {
    page.meta_description = meta_match[1].str();
  }

  // Extract headings (h1-h6)
  for (int level = 1; level <= 6; ++level) {
    std::string tag = "h" + std::to_string(level);
    auto headings = ExtractByTag(html, tag);
    for (auto& h : headings) {
      page.headings.push_back(StripHTMLTags(h));
    }
  }

  // Extract links
  page.links = ExtractLinks(html);

  // Extract buttons
  page.buttons = ExtractButtons(html);

  // Extract forms
  page.forms = ExtractForms(html);

  // Extract main content
  page.main_content = ExtractMainContent(html);

  // Calculate metrics
  page.total_text_length = static_cast<int>(page.main_content.length());
  page.estimated_tokens = page.total_text_length / 4;  // Rough estimate

  // Truncate if needed
  if (page.estimated_tokens > kMaxOutputTokens) {
    page.main_content = TruncateToTokenLimit(page.main_content, kMaxOutputTokens);
    page.was_truncated = true;
    page.estimated_tokens = kMaxOutputTokens;
  }

  return page;
}

StructuredPage DOMInterpreter::ParseTab(int tab_id) const {
  // TODO: Integration with Chromium's content layer
  // Will use RenderFrameHost::ExecuteJavaScript() to extract DOM
  // or access the DOM tree directly through the content API
  StructuredPage page;
  return page;
}

std::vector<ButtonInfo> DOMInterpreter::ExtractButtons(
    const std::string& html) const {
  std::vector<ButtonInfo> buttons;

  // Match <button> elements
  std::regex btn_re(
      R"(<button([^>]*)>(.*?)</button>)",
      std::regex::icase | std::regex::optimize);

  auto begin = std::sregex_iterator(html.begin(), html.end(), btn_re);
  auto end = std::sregex_iterator();

  for (auto it = begin; it != end; ++it) {
    ButtonInfo btn;
    std::string attrs = (*it)[1].str();
    btn.text = StripHTMLTags((*it)[2].str());

    // Extract type attribute
    std::regex type_re(R"(type=["'](.*?)["'])", std::regex::icase);
    std::smatch type_match;
    if (std::regex_search(attrs, type_match, type_re)) {
      btn.type = type_match[1].str();
    } else {
      btn.type = "button";
    }

    // Extract id attribute
    std::regex id_re(R"(id=["'](.*?)["'])", std::regex::icase);
    std::smatch id_match;
    if (std::regex_search(attrs, id_match, id_re)) {
      btn.id = id_match[1].str();
    }

    // Extract aria-label
    std::regex aria_re(R"(aria-label=["'](.*?)["'])", std::regex::icase);
    std::smatch aria_match;
    if (std::regex_search(attrs, aria_match, aria_re)) {
      btn.aria_label = aria_match[1].str();
    }

    btn.is_submit = (btn.type == "submit");
    buttons.push_back(btn);
  }

  // Also match <input type="submit"> and <input type="button">
  std::regex input_btn_re(
      R"(<input([^>]*type=["'](submit|button)["'][^>]*)>)",
      std::regex::icase | std::regex::optimize);

  begin = std::sregex_iterator(html.begin(), html.end(), input_btn_re);
  for (auto it = begin; it != end; ++it) {
    ButtonInfo btn;
    std::string attrs = (*it)[1].str();
    btn.type = (*it)[2].str();

    std::regex val_re(R"(value=["'](.*?)["'])", std::regex::icase);
    std::smatch val_match;
    if (std::regex_search(attrs, val_match, val_re)) {
      btn.text = val_match[1].str();
    }

    btn.is_submit = (btn.type == "submit");
    buttons.push_back(btn);
  }

  return buttons;
}

std::vector<LinkInfo> DOMInterpreter::ExtractLinks(
    const std::string& html) const {
  std::vector<LinkInfo> links;

  std::regex link_re(
      R"(<a\s([^>]*href=["'](.*?)["'][^>]*)>(.*?)</a>)",
      std::regex::icase | std::regex::optimize);

  auto begin = std::sregex_iterator(html.begin(), html.end(), link_re);
  auto end = std::sregex_iterator();

  for (auto it = begin; it != end; ++it) {
    LinkInfo link;
    link.href = (*it)[2].str();
    link.text = StripHTMLTags((*it)[3].str());

    // Extract aria-label
    std::string attrs = (*it)[1].str();
    std::regex aria_re(R"(aria-label=["'](.*?)["'])", std::regex::icase);
    std::smatch aria_match;
    if (std::regex_search(attrs, aria_match, aria_re)) {
      link.aria_label = aria_match[1].str();
    }

    // Determine if it's a navigation link
    link.is_navigation = (link.href.find('#') != 0 &&
                          link.href.find("javascript:") != 0 &&
                          !link.href.empty());

    links.push_back(link);
  }

  return links;
}

std::vector<FormInfo> DOMInterpreter::ExtractForms(
    const std::string& html) const {
  std::vector<FormInfo> forms;

  std::regex form_re(
      R"(<form([^>]*)>([\s\S]*?)</form>)",
      std::regex::icase | std::regex::optimize);

  auto begin = std::sregex_iterator(html.begin(), html.end(), form_re);
  auto end = std::sregex_iterator();

  for (auto it = begin; it != end; ++it) {
    FormInfo form;
    std::string attrs = (*it)[1].str();
    std::string body = (*it)[2].str();

    // Extract form attributes
    std::regex action_re(R"(action=["'](.*?)["'])", std::regex::icase);
    std::smatch action_match;
    if (std::regex_search(attrs, action_match, action_re)) {
      form.action = action_match[1].str();
    }

    std::regex method_re(R"(method=["'](.*?)["'])", std::regex::icase);
    std::smatch method_match;
    if (std::regex_search(attrs, method_match, method_re)) {
      form.method = method_match[1].str();
    }

    // Extract input fields
    std::regex input_re(R"(<input([^>]*)>)", std::regex::icase);
    auto inp_begin = std::sregex_iterator(body.begin(), body.end(), input_re);
    auto inp_end = std::sregex_iterator();

    for (auto inp_it = inp_begin; inp_it != inp_end; ++inp_it) {
      FormField field;
      std::string input_attrs = (*inp_it)[1].str();

      std::regex name_re(R"(name=["'](.*?)["'])", std::regex::icase);
      std::smatch name_match;
      if (std::regex_search(input_attrs, name_match, name_re)) {
        field.name = name_match[1].str();
      }

      std::regex type_re(R"(type=["'](.*?)["'])", std::regex::icase);
      std::smatch type_match;
      if (std::regex_search(input_attrs, type_match, type_re)) {
        field.type = type_match[1].str();
      } else {
        field.type = "text";
      }

      std::regex ph_re(R"(placeholder=["'](.*?)["'])", std::regex::icase);
      std::smatch ph_match;
      if (std::regex_search(input_attrs, ph_match, ph_re)) {
        field.placeholder = ph_match[1].str();
      }

      field.is_required = (input_attrs.find("required") != std::string::npos);

      form.fields.push_back(field);
    }

    forms.push_back(form);
  }

  return forms;
}

std::string DOMInterpreter::ExtractMainContent(const std::string& html) const {
  std::string content = html;

  // Remove script and style tags
  content = std::regex_replace(content,
      std::regex(R"(<script[\s\S]*?</script>)", std::regex::icase), "");
  content = std::regex_replace(content,
      std::regex(R"(<style[\s\S]*?</style>)", std::regex::icase), "");

  // Remove nav, header, footer elements (typically non-content)
  content = std::regex_replace(content,
      std::regex(R"(<nav[\s\S]*?</nav>)", std::regex::icase), "");
  content = std::regex_replace(content,
      std::regex(R"(<header[\s\S]*?</header>)", std::regex::icase), "");
  content = std::regex_replace(content,
      std::regex(R"(<footer[\s\S]*?</footer>)", std::regex::icase), "");

  // Strip remaining HTML tags
  content = StripHTMLTags(content);

  // Clean whitespace
  content = CleanWhitespace(content);

  return content;
}

std::string DOMInterpreter::ExtractBySelector(
    const std::string& html, const std::string& selector) const {
  // TODO: Full CSS selector support
  // For now, support simple tag and class selectors
  // Production will use Chromium's Blink CSS selector engine
  return "";
}

std::string DOMInterpreter::GetElementAtPosition(int tab_id,
                                                   int x, int y) const {
  // TODO: Integration with Chromium's hit testing
  // Uses RenderWidgetHost::GetRenderWidgetHostViewAtPoint()
  return "";
}

std::string DOMInterpreter::TruncateToTokenLimit(const std::string& content,
                                                   int max_tokens) {
  // Rough estimate: 1 token ≈ 4 characters
  size_t max_chars = static_cast<size_t>(max_tokens) * 4;
  if (content.length() <= max_chars) {
    return content;
  }
  return content.substr(0, max_chars) + "\n[Content truncated...]";
}

std::string DOMInterpreter::StripHTMLTags(const std::string& html) const {
  return std::regex_replace(html, std::regex("<[^>]*>"), "");
}

std::string DOMInterpreter::CleanWhitespace(const std::string& text) const {
  // Collapse multiple whitespace into single spaces
  std::string result = std::regex_replace(text, std::regex("\\s+"), " ");

  // Trim leading/trailing whitespace
  size_t start = result.find_first_not_of(" \t\n\r");
  if (start == std::string::npos) return "";
  size_t end = result.find_last_not_of(" \t\n\r");
  return result.substr(start, end - start + 1);
}

std::vector<std::string> DOMInterpreter::ExtractByTag(
    const std::string& html, const std::string& tag) const {
  std::vector<std::string> results;
  std::regex re("<" + tag + "[^>]*>(.*?)</" + tag + ">",
                std::regex::icase);
  auto begin = std::sregex_iterator(html.begin(), html.end(), re);
  auto end = std::sregex_iterator();
  for (auto it = begin; it != end; ++it) {
    results.push_back((*it)[1].str());
  }
  return results;
}

// ---- StructuredPage serialization ----

std::string StructuredPage::ToJSON() const {
  std::ostringstream json;
  json << "{\n";
  json << "  \"page_title\": \"" << page_title << "\",\n";
  json << "  \"page_url\": \"" << page_url << "\",\n";
  json << "  \"meta_description\": \"" << meta_description << "\",\n";

  json << "  \"headings\": [";
  for (size_t i = 0; i < headings.size(); ++i) {
    json << "\"" << headings[i] << "\"";
    if (i < headings.size() - 1) json << ", ";
  }
  json << "],\n";

  json << "  \"buttons\": [";
  for (size_t i = 0; i < buttons.size(); ++i) {
    json << "{\"text\": \"" << buttons[i].text << "\", "
         << "\"type\": \"" << buttons[i].type << "\"}";
    if (i < buttons.size() - 1) json << ", ";
  }
  json << "],\n";

  json << "  \"links\": [";
  for (size_t i = 0; i < links.size() && i < 20; ++i) {
    json << "{\"text\": \"" << links[i].text << "\", "
         << "\"href\": \"" << links[i].href << "\"}";
    if (i < links.size() - 1 && i < 19) json << ", ";
  }
  json << "],\n";

  json << "  \"forms\": [";
  for (size_t i = 0; i < forms.size(); ++i) {
    json << "{\"action\": \"" << forms[i].action << "\", "
         << "\"fields\": " << forms[i].fields.size() << "}";
    if (i < forms.size() - 1) json << ", ";
  }
  json << "],\n";

  json << "  \"main_content\": \"" << main_content.substr(0, 500) << "\",\n";
  json << "  \"estimated_tokens\": " << estimated_tokens << ",\n";
  json << "  \"was_truncated\": " << (was_truncated ? "true" : "false") << "\n";
  json << "}\n";

  return json.str();
}

std::string StructuredPage::ToCompactText() const {
  std::ostringstream text;
  text << "PAGE: " << page_title << "\n";
  text << "URL: " << page_url << "\n";

  if (!headings.empty()) {
    text << "HEADINGS: ";
    for (size_t i = 0; i < headings.size() && i < 10; ++i) {
      text << headings[i];
      if (i < headings.size() - 1) text << " | ";
    }
    text << "\n";
  }

  if (!buttons.empty()) {
    text << "BUTTONS: ";
    for (size_t i = 0; i < buttons.size() && i < 10; ++i) {
      text << "[" << buttons[i].text << "]";
      if (i < buttons.size() - 1) text << " ";
    }
    text << "\n";
  }

  text << "CONTENT:\n" << main_content.substr(0, 2000) << "\n";

  return text.str();
}

}  // namespace molt_ai
