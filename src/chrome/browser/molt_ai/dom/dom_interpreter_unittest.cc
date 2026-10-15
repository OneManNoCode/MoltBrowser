// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/dom/dom_interpreter.h"

#include <string>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace {

StructuredPage Parse(const std::string& html) {
  return DOMInterpreter().ParseHTML(html, "https://example.com/");
}

// What the std::regex implementation returned for an ordinary page; the
// linear-time scanner must return the same (S5).
TEST(DOMInterpreterTest, ParsesAnOrdinaryPage) {
  const StructuredPage p = Parse(
      "<html><head><TITLE lang=en>Shop &amp; more</TITLE>\n"
      "<meta name=\"description\" content=\"Best deals\">\n"
      "<script>var s = '<a href=\"x\">no</a>';</script>\n"
      "<style>a>b{}</style></head><body>\n"
      "<nav><a href=\"/home\">Home</a></nav>\n"
      "<h1 class=t>Deals <b>today</b></h1><h2>Sub</h2>\n"
      "<a class=x href='/item/1' aria-label=\"First\">Item <i>one</i></a>\n"
      "<A HREF=\"#top\">Top</A> <a href=\"javascript:go()\">Go</a>\n"
      "<button type=\"submit\" id=\"buy\" aria-label=\"Buy now\">Buy</button>\n"
      "<button>Plain</button>\n"
      "<form action=\"/search\" method='get'>\n"
      "<input name=\"q\" placeholder=\"Search\" required>\n"
      "<input type=\"submit\" value=\"Find\">\n"
      "</form>\n"
      "<footer>Footer text</footer></body></html>");
  EXPECT_EQ(p.page_title, "Shop &amp; more");
  EXPECT_EQ(p.meta_description, "Best deals");
  ASSERT_EQ(p.headings.size(), 2u);
  EXPECT_EQ(p.headings[0], "Deals today");
  EXPECT_EQ(p.headings[1], "Sub");

  // The <a> inside the <script> string counts too (the regex didn't
  // understand scripts either); the <nav> link does as well.
  ASSERT_EQ(p.links.size(), 5u);
  EXPECT_EQ(p.links[0].href, "x");
  EXPECT_EQ(p.links[1].href, "/home");
  EXPECT_EQ(p.links[2].href, "/item/1");
  EXPECT_EQ(p.links[2].text, "Item one");
  EXPECT_EQ(p.links[2].aria_label, "First");
  EXPECT_TRUE(p.links[2].is_navigation);
  EXPECT_EQ(p.links[3].href, "#top");
  EXPECT_FALSE(p.links[3].is_navigation);
  EXPECT_FALSE(p.links[4].is_navigation);  // javascript:

  ASSERT_EQ(p.buttons.size(), 3u);
  EXPECT_EQ(p.buttons[0].text, "Buy");
  EXPECT_EQ(p.buttons[0].type, "submit");
  EXPECT_EQ(p.buttons[0].id, "buy");
  EXPECT_EQ(p.buttons[0].aria_label, "Buy now");
  EXPECT_TRUE(p.buttons[0].is_submit);
  EXPECT_EQ(p.buttons[1].type, "button");
  EXPECT_EQ(p.buttons[2].text, "Find");  // <input type="submit">
  EXPECT_TRUE(p.buttons[2].is_submit);

  ASSERT_EQ(p.forms.size(), 1u);
  EXPECT_EQ(p.forms[0].action, "/search");
  EXPECT_EQ(p.forms[0].method, "get");
  ASSERT_EQ(p.forms[0].fields.size(), 2u);
  EXPECT_EQ(p.forms[0].fields[0].name, "q");
  EXPECT_EQ(p.forms[0].fields[0].type, "text");
  EXPECT_EQ(p.forms[0].fields[0].placeholder, "Search");
  EXPECT_TRUE(p.forms[0].fields[0].is_required);
  EXPECT_EQ(p.forms[0].fields[1].type, "submit");

  // Scripts, styles, nav and footer are dropped; tags stripped (without a
  // space, so "</h1><h2>" glues "today" and "Sub"); whitespace collapsed.
  EXPECT_EQ(p.main_content,
            "Shop &amp; more Deals todaySub Item one Top Go Buy Plain");
  EXPECT_FALSE(p.was_truncated);
}

// Regex quirks the scanner keeps.
TEST(DOMInterpreterTest, KeepsTheRegexEdgeCases) {
  // Link text may not cross a line break.
  EXPECT_TRUE(Parse("<a href=\"x\">a\nb</a>").links.empty());
  // An href value can run past '>' up to a quote that completes the match.
  StructuredPage p = Parse("<a href=\"a>b\">c</a>");
  ASSERT_EQ(p.links.size(), 1u);
  EXPECT_EQ(p.links[0].href, "a>b");
  // Of several href attributes, the last one in the tag is taken.
  p = Parse("<a href=\"x\" href=\"y\">z</a>");
  ASSERT_EQ(p.links.size(), 1u);
  EXPECT_EQ(p.links[0].href, "y");
  // Unquoted href: not a link.
  EXPECT_TRUE(Parse("<a href=x>n</a>").links.empty());
  // <input>: the last type="submit|button" before '>' decides.
  p = Parse("<input type='button' type=\"submit\" value=v>");
  ASSERT_EQ(p.buttons.size(), 1u);
  EXPECT_EQ(p.buttons[0].type, "submit");
  EXPECT_EQ(p.buttons[0].text, "");  // value=v is unquoted
  // A title whose close is on the next line isn't one.
  EXPECT_EQ(Parse("<title>a\n</title>").page_title, "");
  // A form body may span lines; an unclosed form isn't a form.
  EXPECT_EQ(Parse("<form>\n<input name='a'>\n</form>").forms.size(), 1u);
  EXPECT_TRUE(Parse("<form><input name='a'>").forms.empty());
}

// Inputs that made the std::regex version take quadratic time or throw
// regex_error (an abort under -fno-exceptions): now quick and harmless.
TEST(DOMInterpreterTest, HostilePagesAreHarmless) {
  EXPECT_TRUE(
      Parse("<a href=\"" + std::string(64 * 1024, '"') + ">").links.empty());
  std::string unclosed;
  while (unclosed.size() < 256 * 1024) {
    unclosed += "<a x><form><button><title>";
  }
  const StructuredPage p = Parse(unclosed);
  EXPECT_TRUE(p.links.empty());
  EXPECT_TRUE(p.forms.empty());
  std::string hrefs = "<a ";
  while (hrefs.size() < 256 * 1024) {
    hrefs += "href=\"x\" ";
  }
  EXPECT_EQ(Parse(hrefs + ">t</a>").links.size(), 1u);
}

TEST(DOMInterpreterTest, CapHtmlForParsing) {
  const size_t cap = DOMInterpreter::kMaxParseHtmlBytes;
  EXPECT_EQ(DOMInterpreter::CapHtmlForParsing("<p>hi</p>"), "<p>hi</p>");
  EXPECT_EQ(DOMInterpreter::CapHtmlForParsing(std::string(cap, 'x')).size(),
            cap);
  EXPECT_EQ(
      DOMInterpreter::CapHtmlForParsing(std::string(cap + 10, 'x')).size(),
      cap);
  // A 3-byte character straddling the cap is dropped whole.
  std::string html(cap - 1, 'x');
  html += "\xe2\x82\xac" "tail";  // U+20AC
  const std::string capped = DOMInterpreter::CapHtmlForParsing(html);
  EXPECT_EQ(capped.size(), cap - 1);
  EXPECT_EQ(capped.back(), 'x');
}

}  // namespace
}  // namespace molt_ai
