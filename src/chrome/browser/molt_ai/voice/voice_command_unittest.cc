// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_command.h"

#include <chrono>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/voice/voice_command_phrases.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice {
namespace {

enum Ctx { kNone, kYesNo, kList, kField };

ParseContext MakeContext(Ctx ctx) {
  ParseContext context;
  context.expecting_yes_no = ctx == kYesNo;
  context.expecting_choice = ctx == kList;
  context.text_field_focused = ctx == kField;
  return context;
}

std::string ChoiceText(const ChoiceRef& ref) {
  std::string out;
  switch (ref.kind) {
    case ChoiceRef::Kind::kNone:
      return "";
    case ChoiceRef::Kind::kPosition:
      out = "pos:" + std::string(ref.from_end ? "-" : "") +
            std::to_string(ref.ordinal);
      break;
    case ChoiceRef::Kind::kNumber:
      out = "num:" + std::to_string(ref.number);
      break;
    case ChoiceRef::Kind::kRelative:
      out = ref.relative == ChoiceRef::Relative::kCurrent ? "rel:current"
            : ref.relative == ChoiceRef::Relative::kNext  ? "rel:next"
                                                          : "rel:previous";
      break;
    case ChoiceRef::Kind::kDescription: {
      out = "desc:";
      for (size_t i = 0; i < ref.words.size(); ++i) {
        out += (i ? "|" : "") + ref.words[i];
      }
      if (ref.ordinal) out += "#" + std::to_string(ref.ordinal);
      if (!ref.by_words.empty()) {
        out += ",by:";
        for (size_t i = 0; i < ref.by_words.size(); ++i) {
          out += (i ? "|" : "") + ref.by_words[i];
        }
      }
      if (!ref.from_words.empty()) {
        out += ",from:";
        for (size_t i = 0; i < ref.from_words.size(); ++i) {
          out += (i ? "|" : "") + ref.from_words[i];
        }
      }
      break;
    }
  }
  return out;
}

// The command's fields that differ from their defaults, as key=value.
std::map<std::string, std::string> Fields(const VoiceCommand& c) {
  std::map<std::string, std::string> f;
  f["kind"] = std::string(VoiceCommandKindName(c.kind));
  if (!c.text.empty()) f["text"] = c.text;
  if (!c.site.empty()) f["site"] = c.site;
  if (c.site_is_address) f["address"] = "1";
  if (c.new_tab) f["new_tab"] = "1";
  if (c.kind == VoiceCommandKind::kSearch) {
    static constexpr std::string_view kScopes[] = {"auto", "current_site",
                                                   "default_engine", "named"};
    f["scope"] = std::string(kScopes[static_cast<int>(c.scope)]);
  }
  if (!c.whole_query.empty()) f["whole_query"] = c.whole_query;
  if (c.kind == VoiceCommandKind::kChoose) {
    static constexpr std::string_view kVerbs[] = {
        "default", "open", "play", "click", "select", "read", "describe"};
    f["verb"] = std::string(kVerbs[static_cast<int>(c.verb)]);
  }
  const std::string choice = ChoiceText(c.choice);
  if (!choice.empty()) f["choice"] = choice;
  if (c.choice.roles) f["roles"] = std::to_string(c.choice.roles);
  if (c.seconds) f["seconds"] = std::to_string(c.seconds);
  if (c.kind == VoiceCommandKind::kSetVolume) {
    f["level"] = std::to_string(c.level);
  }
  if (c.scroll != ScrollAmount::kDefault) {
    f["scroll"] = c.scroll == ScrollAmount::kPage ? "page" : "half";
  }
  if (c.count != 1) f["count"] = std::to_string(c.count);
  if (c.kind == VoiceCommandKind::kVerbosity) {
    static constexpr std::string_view kLevels[] = {"brief", "normal", "full",
                                                   "more", "less"};
    f["verbosity"] = std::string(kLevels[static_cast<int>(c.verbosity)]);
  }
  if (c.refusal == RefusalReason::kTooLong) f["refusal"] = "too_long";
  if (c.refusal == RefusalReason::kInvalidUtf8) f["refusal"] = "utf8";
  return f;
}

std::string Show(const std::map<std::string, std::string>& fields) {
  std::string out;
  for (const auto& [key, value] : fields) out += key + "=" + value + "; ";
  return out;
}

struct Case {
  std::string_view utterance;
  std::string_view kind;
  // "key=value;key=value": fields that must be there with these values.
  std::string_view expect = {};
  Ctx ctx = kNone;
};

void Check(const std::vector<Case>& cases) {
  for (const Case& c : cases) {
    const VoiceCommand command =
        ParseVoiceCommand(c.utterance, MakeContext(c.ctx));
    const std::map<std::string, std::string> fields = Fields(command);
    SCOPED_TRACE(std::string(c.utterance) + "  ->  " + Show(fields));
    EXPECT_EQ(fields.at("kind"), c.kind);
    std::string_view expect = c.expect;
    while (!expect.empty()) {
      const size_t semi = expect.find(';');
      const std::string_view pair = expect.substr(0, semi);
      const size_t eq = pair.find('=');
      ASSERT_NE(eq, std::string_view::npos) << pair;
      const std::string key(pair.substr(0, eq));
      const std::string value(pair.substr(eq + 1));
      const auto found = fields.find(key);
      if (found == fields.end()) {
        ADD_FAILURE() << "missing " << key;
      } else {
        EXPECT_EQ(found->second, value) << key;
      }
      if (semi == std::string_view::npos) break;
      expect.remove_prefix(semi + 1);
    }
  }
}

TEST(VoiceCommandTest, OpenSites) {
  Check({
      {"open YouTube", "open_site", "site=youtube"},
      {"Hey Molt, open YouTube.", "open_site", "site=youtube"},
      {"hey mold open youtube", "open_site", "site=youtube"},
      {"OK Molt go to youtube", "open_site", "site=youtube"},
      {"go to example dot com", "open_site", "site=example.com;address=1"},
      {"go to example.com", "open_site", "site=example.com;address=1"},
      {"example dot com", "open_site", "site=example.com;address=1"},
      {"take me to the new york times", "open_site", "site=newyorktimes"},
      {"visit w w w dot bbc dot co dot uk", "open_site",
       "site=www.bbc.co.uk"},
      {"open b b c dot com", "open_site", "site=bbc.com"},
      {"navigate to wikipedia", "open_site", "site=wikipedia"},
      {"open the youtube website", "open_site", "site=youtube"},
      {"open youtube in a new tab", "open_site", "site=youtube;new_tab=1"},
      {"go to my-site.com", "open_site", "site=my-site.com"},
      {"could you please open gmail", "open_site", "site=gmail"},
      {"um open uh youtube please", "open_site", "site=youtube"},
      {"I want to go to amazon", "open_site", "site=amazon"},
      {"open amazon dot co dot uk", "open_site", "site=amazon.co.uk"},
      {"go to 9gag dot com", "open_site", "site=9gag.com"},
      {"launch spotify", "open_site", "site=spotify"},
      {"open the guardian", "open_site", "site=guardian"},
      {"go to youtube.com/feed", "open_site", "site=youtube.com/feed"},
      {"open example dot com slash news", "open_site",
       "site=example.com/news"},
      {"OPEN YOUTUBE", "open_site", "site=youtube"},
      {"Open YouTube!", "open_site", "site=youtube"},
      {"open you tube", "open_site", "site=youtube"},
      {"go to settings", "open_site", "site=settings"},
      {"go to youtube please thank you", "open_site", "site=youtube"},
      {"take me to reddit dot com", "open_site", "site=reddit.com"},
      {"open bohemian rhapsody", "open_site", "site=bohemianrhapsody"},
      {"pull up the weather channel", "open_site", "site=weatherchannel"},
      {"bring up example dot org", "open_site", "site=example.org"},
      {"open news.ycombinator.com", "open_site",
       "site=news.ycombinator.com"},
      {"go to example underscore site dot com", "open_site",
       "site=example_site.com"},
  });
}

TEST(VoiceCommandTest, Search) {
  Check({
      {"search for Bohemian Rhapsody", "search",
       "scope=auto;text=Bohemian Rhapsody"},
      {"Search for Bohemian Rhapsody.", "search", "text=Bohemian Rhapsody"},
      {"search bohemian rhapsody", "search", "scope=auto"},
      {"search YouTube for Bohemian Rhapsody", "search",
       "scope=named;site=youtube;text=Bohemian Rhapsody"},
      {"search Google for the weather in Paris", "search",
       "scope=named;site=google;text=the weather in Paris"},
      {"search for cats on youtube", "search",
       "scope=named;site=youtube;text=cats;whole_query=cats on youtube"},
      {"search for things to do in paris on youtube", "search",
       "site=youtube;text=things to do in paris"},
      {"search the web for train times", "search",
       "scope=default_engine;text=train times"},
      {"search online for pizza near me", "search",
       "scope=default_engine;text=pizza near me"},
      {"web search best headphones", "search",
       "scope=default_engine;text=best headphones"},
      {"search this site for returns policy", "search",
       "scope=current_site;text=returns policy"},
      {"search for returns policy on this site", "search",
       "scope=current_site;text=returns policy"},
      {"look up the eiffel tower", "search",
       "scope=auto;text=the eiffel tower"},
      {"look up quantum physics on wikipedia", "search",
       "scope=named;site=wikipedia;text=quantum physics"},
      {"search for apocalypse now", "search", "text=apocalypse now"},
      {"search for AC/DC", "search", "text=AC/DC"},
      {"search for node.js tutorials", "search", "text=node.js tutorials"},
      {"search for um bohemian rhapsody", "search",
       "text=bohemian rhapsody"},
      {"can you search for cheap flights please", "search",
       "text=cheap flights"},
      {"search for rock & roll", "search", "text=rock & roll"},
      {"look for running shoes", "search", "text=running shoes"},
      {"search for 50% off deals", "search", "text=50% off deals"},
      {"search for Beyonc\xC3\xA9", "search", "text=Beyonc\xC3\xA9"},
      {"search the internet for how to tie a tie", "search",
       "scope=default_engine;text=how to tie a tie"},
      {"find opening hours on this page", "find_in_page",
       "text=opening hours"},
      {"find on page refund", "find_in_page", "text=refund"},
      {"search this page for price", "find_in_page", "text=price"},
      {"find the word shipping on the page", "find_in_page",
       "text=shipping"},
  });
}

TEST(VoiceCommandTest, BackForwardReloadHome) {
  Check({
      {"go back", "back"},
      {"back", "back"},
      {"go back a page", "back"},
      {"previous page", "back"},
      {"Go back.", "back"},
      {"take me back", "back"},
      {"go back to the previous page", "back"},
      {"navigate back", "back"},
      {"go forward", "forward"},
      {"forward", "forward"},
      {"go forward one page", "forward"},
      {"reload", "reload"},
      {"refresh the page", "reload"},
      {"reload this page", "reload"},
      {"refresh", "reload"},
      {"load the page again", "reload"},
      {"stop loading", "stop_loading"},
      {"stop loading the page", "stop_loading"},
      {"go home", "home"},
      {"home page", "home"},
      {"next page", "next_page"},
      {"go to the next page", "next_page"},
      {"more results", "next_page"},
  });
}

TEST(VoiceCommandTest, Tabs) {
  Check({
      {"new tab", "new_tab"},
      {"open a new tab", "new_tab"},
      {"open new tab", "new_tab"},
      {"open another tab", "new_tab"},
      {"close tab", "close_tab"},
      {"close this tab", "close_tab"},
      {"close the current tab", "close_tab"},
      {"close tab 3", "close_tab", "choice=num:3"},
      {"close the third tab", "close_tab", "choice=pos:3"},
      {"close the youtube tab", "close_tab", "choice=desc:youtube"},
      {"switch to tab 2", "switch_tab", "choice=num:2"},
      {"go to tab two", "switch_tab", "choice=num:2"},
      {"tab 3", "switch_tab", "choice=num:3"},
      {"switch to the youtube tab", "switch_tab", "choice=desc:youtube"},
      {"switch to youtube", "switch_tab", "choice=desc:youtube"},
      {"next tab", "switch_tab", "choice=rel:next"},
      {"go to the previous tab", "switch_tab", "choice=rel:previous"},
      {"switch to the last tab", "switch_tab", "choice=pos:-1"},
      {"go to tab too", "switch_tab", "choice=num:2"},
      {"switch to tab for", "switch_tab", "choice=num:4"},
      {"switch to the first tab", "switch_tab", "choice=pos:1"},
  });
}

TEST(VoiceCommandTest, ScrollAndZoom) {
  Check({
      {"scroll down", "scroll_down"},
      {"scroll up", "scroll_up"},
      {"page down", "scroll_down", "scroll=page"},
      {"page up", "scroll_up", "scroll=page"},
      {"scroll down a bit", "scroll_down", "scroll=half"},
      {"scroll down half a page", "scroll_down", "scroll=half"},
      {"scroll down a page", "scroll_down", "scroll=page"},
      {"scroll down two pages", "scroll_down", "scroll=page;count=2"},
      {"scroll down 3 pages", "scroll_down", "scroll=page;count=3"},
      {"scroll down to pages", "scroll_down", "scroll=page;count=2"},
      {"down", "scroll_down"},
      {"up", "scroll_up"},
      {"scroll", "scroll_down"},
      {"keep scrolling", "scroll_down"},
      {"scroll up a little", "scroll_up", "scroll=half"},
      {"go down a page", "scroll_down", "scroll=page"},
      {"scroll to the top", "scroll_to_top"},
      {"go to the top", "scroll_to_top"},
      {"go back to the top", "scroll_to_top"},
      {"top of the page", "scroll_to_top"},
      {"scroll up to the top of the page", "scroll_to_top"},
      {"scroll to the bottom", "scroll_to_bottom"},
      {"go to the end of the page", "scroll_to_bottom"},
      {"bottom of page", "scroll_to_bottom"},
      {"scroll left", "scroll_left"},
      {"scroll right", "scroll_right"},
      {"zoom in", "zoom_in"},
      {"zoom out", "zoom_out"},
      {"reset zoom", "zoom_reset"},
      {"make it bigger", "zoom_in"},
      {"bigger text", "zoom_in"},
      {"make the text smaller", "zoom_out"},
      {"actual size", "zoom_reset"},
      {"zoom back to normal", "zoom_reset"},
      {"larger", "zoom_in"},
      {"smaller", "zoom_out"},
      {"magnify", "zoom_in"},
  });
}

TEST(VoiceCommandTest, Media) {
  Check({
      {"play", "play"},
      {"pause", "pause"},
      {"pause the video", "pause"},
      {"resume", "resume"},
      {"continue", "resume"},
      {"unpause", "resume"},
      {"keep playing", "resume"},
      {"louder", "volume_up"},
      {"volume up", "volume_up"},
      {"turn it up", "volume_up"},
      {"a bit louder", "volume_up"},
      {"quieter", "volume_down"},
      {"volume down", "volume_down"},
      {"turn it down", "volume_down"},
      {"softer", "volume_down"},
      {"volume 5", "set_volume", "level=50"},
      {"set the volume to 50 percent", "set_volume", "level=50"},
      {"volume to 80%", "set_volume", "level=80"},
      {"set volume to max", "set_volume", "level=100"},
      {"volume half", "set_volume", "level=50"},
      {"volume zero", "set_volume", "level=0"},
      {"volume 7 out of 10", "set_volume", "level=70"},
      {"volume 100", "set_volume", "level=100"},
      {"volume 150", "unknown"},
      {"turn the volume to thirty", "set_volume", "level=30"},
      {"mute", "mute"},
      {"mute the sound", "mute"},
      {"unmute", "unmute"},
      {"sound on", "unmute"},
      {"sound off", "mute"},
      {"skip ahead one minute", "skip_forward", "seconds=60"},
      {"skip forward 30 seconds", "skip_forward", "seconds=30"},
      {"skip ahead", "skip_forward"},
      {"skip", "skip_forward"},
      {"fast forward", "skip_forward"},
      {"fast forward 2 minutes", "skip_forward", "seconds=120"},
      {"go forward 10 seconds", "skip_forward", "seconds=10"},
      {"jump ahead a minute and a half", "skip_forward", "seconds=90"},
      {"skip ahead by 1 minute 30 seconds", "skip_forward", "seconds=90"},
      {"skip ahead to minutes", "skip_forward", "seconds=120"},
      {"skip forward for seconds", "skip_forward", "seconds=4"},
      {"rewind", "skip_back"},
      {"rewind 10 seconds", "skip_back", "seconds=10"},
      {"go back 10 seconds", "skip_back", "seconds=10"},
      {"skip back 30 secs", "skip_back", "seconds=30"},
      {"back 5 seconds", "skip_back", "seconds=5"},
      {"jump back half a minute", "skip_back", "seconds=30"},
      {"go back one and a half minutes", "skip_back", "seconds=90"},
      {"restart", "restart"},
      {"start over", "restart"},
      {"play it again", "restart"},
      {"from the beginning", "restart"},
      {"rewind to the start", "restart"},
      {"next video", "next_track"},
      {"next song", "next_track"},
      {"play the next video", "next_track"},
      {"skip this song", "next_track"},
      {"previous video", "previous_track"},
      {"go back to the previous song", "previous_track"},
      {"full screen", "full_screen"},
      {"fullscreen", "full_screen"},
      {"go full screen", "full_screen"},
      {"full-screen", "full_screen"},
      {"make it full screen", "full_screen"},
      {"exit full screen", "exit_full_screen"},
      {"leave fullscreen", "exit_full_screen"},
      {"small screen", "exit_full_screen"},
      {"turn on captions", "captions_on"},
      {"captions on", "captions_on"},
      {"subtitles on", "captions_on"},
      {"turn off subtitles", "captions_off"},
      {"captions off", "captions_off"},
      {"hide captions", "captions_off"},
      {"speed up", "speed_up"},
      {"faster", "speed_up"},
      {"play faster", "speed_up"},
      {"slow down", "slow_down"},
      {"slower", "slow_down"},
      {"normal speed", "normal_speed"},
      {"reset the speed", "normal_speed"},
      {"Hey Molt, louder.", "volume_up"},
      {"Hey Molt, pause.", "pause"},
      {"Hey Molt, skip ahead one minute.", "skip_forward", "seconds=60"},
      {"Hey Molt, full screen.", "full_screen"},
      {"Hey Molt, go back.", "back"},
      {"stop the video", "pause"},
      {"hit play", "play"},
      {"play it", "play"},
      {"pause it", "pause"},
      {"mute it", "mute"},
  });
}

TEST(VoiceCommandTest, Reading) {
  Check({
      {"read the page", "read_page"},
      {"read this page", "read_page"},
      {"read it to me", "read_page"},
      {"read aloud", "read_page"},
      {"read", "read_page"},
      {"read the whole article", "read_page"},
      {"read from here", "read_from_here"},
      {"continue reading", "read_from_here"},
      {"keep reading", "read_from_here"},
      {"read the headings", "read_headings"},
      {"list the headings", "read_headings"},
      {"what are the headings", "read_headings"},
      {"read the links", "read_links"},
      {"list all the links", "read_links"},
      {"read the results", "read_results"},
      {"read the results again", "read_results"},
      {"what are the results", "read_results"},
      {"what are my options", "read_results"},
      {"read the title", "read_title"},
      {"what's the title", "read_title"},
      {"what is the page title", "read_title"},
      {"read the selection", "read_selection"},
      {"read the selected text", "read_selection"},
      {"read what's selected", "read_selection"},
      {"what's on the screen", "describe_screen"},
      {"what is on the screen", "describe_screen"},
      {"what's on screen", "describe_screen"},
      {"what do you see", "describe_screen"},
      {"describe the screen", "describe_screen"},
      {"next heading", "next_heading"},
      {"previous heading", "previous_heading"},
      {"go to the next heading", "next_heading"},
      {"next link", "next_link"},
      {"previous link", "previous_link"},
      {"next item", "next_item"},
      {"next result", "next_item"},
      {"previous item", "previous_item"},
      {"next", "next_item"},
      {"previous", "previous_item"},
      {"read the next paragraph", "next_item"},
  });
}

TEST(VoiceCommandTest, Choosing) {
  Check({
      {"play the fourth one", "choose", "verb=play;choice=pos:4"},
      {"play the 4th one", "choose", "verb=play;choice=pos:4"},
      {"play number four", "choose", "verb=play;choice=num:4"},
      {"play no. 4", "choose", "verb=play;choice=num:4"},
      {"play #4", "choose", "verb=play;choice=num:4"},
      {"play the forth one", "choose", "choice=pos:4"},
      {"play the last one", "choose", "choice=pos:-1"},
      {"play the second to last one", "choose", "choice=pos:-2"},
      {"play the one before last", "choose", "choice=pos:-2"},
      {"play the live one by Queen", "choose",
       "verb=play;choice=desc:live,by:queen"},
      {"play the live one by queen please", "choose",
       "choice=desc:live,by:queen"},
      {"click 7", "choose", "verb=click;choice=num:7"},
      {"click on the sign in button", "choose",
       "verb=click;choice=desc:sign|in"},
      {"press the search button", "choose", "verb=click;choice=desc:search"},
      {"select the third option", "choose", "verb=select;choice=pos:3"},
      {"choose number 12", "choose", "verb=select;choice=num:12"},
      {"read the third result", "choose", "verb=read;choice=pos:3"},
      {"describe the second one", "choose", "verb=describe;choice=pos:2"},
      {"tell me about the first video", "choose",
       "verb=describe;choice=pos:1"},
      {"what is the fourth one", "choose", "verb=describe;choice=pos:4"},
      {"the fourth one", "choose", "verb=default;choice=pos:4"},
      {"the last one", "choose", "choice=pos:-1"},
      {"number 12", "choose", "choice=num:12"},
      {"No. 12", "choose", "choice=num:12"},
      {"four", "choose", "choice=num:4", kList},
      {"4", "choose", "choice=num:4", kList},
      {"for", "choose", "choice=num:4", kList},
      {"the live one", "choose", "choice=desc:live", kList},
      {"the one by Queen", "choose", "choice=desc:,by:queen", kList},
      {"four", "unknown"},
      {"the next one", "choose", "choice=rel:next"},
      {"this one", "choose", "choice=rel:current"},
      {"play this one", "choose", "verb=play;choice=rel:current"},
      {"click the twenty-first link", "choose", "choice=pos:21"},
      {"open the one hundredth result", "choose", "choice=pos:100"},
      {"play the one hundred and first one", "choose", "choice=pos:101"},
      {"show numbers", "show_numbers"},
      {"hide numbers", "hide_numbers"},
      {"show me the numbers", "show_numbers"},
      {"numbers off", "hide_numbers"},
      {"watch the third video", "choose", "verb=play;choice=pos:3"},
      {"listen to the second one", "choose", "verb=play;choice=pos:2"},
      {"check the remember me box", "choose",
       "verb=click;choice=desc:remember|me"},
      {"open bohemian rhapsody", "choose",
       "verb=open;choice=desc:bohemian|rhapsody", kList},
      {"play bohemian rap city", "choose",
       "verb=play;choice=desc:bohemian|rap|city"},
      {"Bohemian Rhapsody live", "choose",
       "verb=default;choice=desc:bohemian|rhapsody|live", kList},
      {"go to the third one", "choose", "verb=open;choice=pos:3"},
      {"open the live one", "choose", "verb=open;choice=desc:live"},
      {"open it", "choose", "verb=open;choice=rel:current"},
      {"go to number 5", "choose", "verb=open;choice=num:5"},
      {"open the first link", "choose", "verb=open;choice=pos:1"},
      {"play the second live one", "choose", "choice=desc:live#2"},
      {"play the one from nineteen eighty five", "choose",
       "choice=desc:,from:1985"},
      {"pick number too", "choose", "verb=select;choice=num:2"},
      {"click won", "choose", "verb=click;choice=num:1"},
      {"the one above", "choose", "choice=rel:previous"},
      {"play the very last one", "choose", "choice=pos:-1"},
      {"select the next to last one", "choose", "choice=pos:-2"},
  });
}

TEST(VoiceCommandTest, Dictation) {
  Check({
      {"type hello world", "type", "text=hello world"},
      {"type Hello, World!", "type", "text=Hello, World"},
      {"write dear john", "type", "text=dear john"},
      {"type um hello", "type", "text=hello"},
      {"type search for cats", "type", "text=search for cats"},
      {"type I'll be late", "type", "text=I'll be late"},
      {"dictate meeting notes", "type", "text=meeting notes"},
      {"type see you now", "type", "text=see you now"},
      {"new line", "new_line", "", kField},
      {"new line", "unknown"},
      {"new paragraph", "new_line", "", kField},
      {"delete the last word", "delete_last_word", "", kField},
      {"delete that word", "delete_last_word", "", kField},
      {"clear the field", "clear_field", "", kField},
      {"clear", "clear_field", "", kField},
      {"clear everything", "clear_field", "", kField},
      {"read it back", "read_back", "", kField},
      {"what did I type", "read_back", "", kField},
      {"submit", "submit", "", kField},
      {"send", "submit", "", kField},
      {"search", "submit", "", kField},
      {"press enter", "submit"},
      {"submit the form", "submit"},
  });
}

TEST(VoiceCommandTest, Conversation) {
  Check({
      {"yes", "yes", "", kYesNo},
      {"yeah", "yes", "", kYesNo},
      {"yes please", "yes", "", kYesNo},
      {"sure", "yes", "", kYesNo},
      {"ok", "yes", "", kYesNo},
      {"go ahead", "yes", "", kYesNo},
      {"do it", "yes", "", kYesNo},
      {"yes do it", "yes", "", kYesNo},
      {"that's right", "yes", "", kYesNo},
      {"no", "no", "", kYesNo},
      {"nope", "no", "", kYesNo},
      {"no thanks", "no", "", kYesNo},
      {"don't", "no", "", kYesNo},
      {"no don't do it", "no", "", kYesNo},
      {"yes", "unknown"},
      {"no", "unknown"},
      {"cancel", "cancel"},
      {"never mind", "cancel"},
      {"forget it", "cancel"},
      {"cancel", "cancel", "", kYesNo},
      {"stop", "stop"},
      {"stop it", "stop"},
      {"stop talking", "stop_speaking"},
      {"be quiet", "stop_speaking"},
      {"shut up", "stop_speaking"},
      {"stop reading", "stop_speaking"},
      {"undo", "undo"},
      {"undo that", "undo"},
      {"repeat", "repeat"},
      {"say that again", "repeat"},
      {"what did you say", "repeat"},
      {"help", "help"},
      {"what can I say", "help"},
      {"What can I say?", "help"},
      {"where am I", "where_am_i"},
      {"what page is this", "where_am_i"},
      {"speak faster", "speak_faster"},
      {"talk slower", "speak_slower"},
      {"speak more slowly", "speak_slower"},
      {"be brief", "verbosity", "verbosity=brief"},
      {"full detail", "verbosity", "verbosity=full"},
      {"more detail", "verbosity", "verbosity=more"},
      {"less detail", "verbosity", "verbosity=less"},
      {"normal verbosity", "verbosity", "verbosity=normal"},
      {"no, I said YouTube", "correction", "text=YouTube"},
      {"No I said open YouTube", "correction", "text=open YouTube"},
      {"I meant the third one", "correction", "text=the third one"},
      {"no I said YouTube", "correction", "text=YouTube", kYesNo},
      {"nope I meant bohemian rhapsody", "correction",
       "text=bohemian rhapsody"},
  });
}

TEST(VoiceCommandTest, Routines) {
  Check({
      {"remember this as my news", "remember_routine", "text=my news"},
      {"save this as morning routine", "remember_routine",
       "text=morning routine"},
      {"remember this as a routine called daily news", "remember_routine",
       "text=daily news"},
      {"forget my news", "forget_routine", "text=my news"},
      {"delete the routine called my news", "forget_routine",
       "text=my news"},
      {"run my news", "run_routine", "text=my news"},
      {"run the routine called morning", "run_routine", "text=morning"},
      {"Hey Molt, run my news.", "run_routine", "text=my news"},
      // The grammar never guesses that a bare name is a routine: the
      // browser checks its routines before asking the planner.
      {"my news", "unknown"},
  });
}

TEST(VoiceCommandTest, UnknownGoesToThePlanner) {
  Check({
      {"find a train from Paris to Lyon tomorrow morning", "unknown"},
      {"reply to the last email from Ana saying I'll be late", "unknown"},
      {"what does this page say about prices", "unknown"},
      {"summarize this article", "unknown"},
      {"hello", "unknown"},
      {"", "unknown"},
      {"um uh", "unknown"},
      {"hey molt", "unknown"},
      {"please", "unknown"},
      {"youtube", "unknown"},
      {"what's the weather like in Lisbon", "choose", "verb=describe"},
  });
  const VoiceCommand planner = ParseVoiceCommand(
      "Hey Molt, find a train from Paris to Lyon tomorrow morning.",
      ParseContext());
  EXPECT_EQ(planner.kind, VoiceCommandKind::kUnknown);
  EXPECT_EQ(planner.heard, "find a train from Paris to Lyon tomorrow morning");
}

TEST(VoiceCommandTest, RecognizerForms) {
  Check({
      {"play the fourth one\x07", "choose", "choice=pos:4"},
      {"play the\xE2\x80\xAE fourth one", "choose", "choice=pos:4"},
      {"PLAY THE FOURTH ONE", "choose", "choice=pos:4"},
      {"play   the   fourth   one", "choose", "choice=pos:4"},
      {"Play the fourth one?", "choose", "choice=pos:4"},
      {"play the 4TH one", "choose", "choice=pos:4"},
      {"play the fourth one now", "choose", "choice=pos:4"},
      {"could you play the fourth one for me please", "choose",
       "choice=pos:4"},
      {"um play the uh fourth one", "choose", "choice=pos:4"},
      {"hey molt um play the fourth one", "choose", "choice=pos:4"},
      {"Hey, Molt! Play the fourth one.", "choose", "choice=pos:4"},
      {"okay louder", "volume_up"},
      {"so go back", "back"},
      {"and then scroll down", "scroll_down"},
      {"please please go back", "back"},
      {"play the fourth one, please", "choose", "choice=pos:4"},
      {"I'd like to go back", "back"},
      {"can you please turn it up", "volume_up"},
      {"would you mute", "mute"},
      {"let's go full screen", "full_screen"},
      {"just pause", "pause"},
      {"go ahead and play", "play"},
      {"play\xEF\xBB\xBF the fourth one", "choose", "choice=pos:4"},
      {"play the fourth\xE2\x80\x8B one", "choose", "choice=pos:4"},
      {"\xEF\xBC\xB0lay the fourth one", "choose", "choice=pos:4"},
      {"play the 2nd one", "choose", "choice=pos:2"},
      {"play the 3rd one", "choose", "choice=pos:3"},
      {"play the 21st one", "choose", "choice=pos:21"},
      {"play number twenty one", "choose", "choice=num:21"},
      {"play number nine hundred ninety nine", "choose", "choice=num:999"},
      {"play the hundredth one", "choose", "choice=pos:100"},
      {"skip ahead 1,000 seconds", "skip_forward", "seconds=1000"},
  });
}

TEST(VoiceCommandTest, Heard) {
  ParseContext context;
  VoiceCommand command = ParseVoiceCommand("Hey Molt, open YouTube.", context);
  EXPECT_EQ(command.heard, "open YouTube");
  EXPECT_EQ(command.site_words, "YouTube");
  command = ParseVoiceCommand("open\x01 the\xE2\x80\x8F New York Times",
                              context);
  EXPECT_EQ(command.heard, "open the New York Times");
  EXPECT_EQ(command.site_words, "the New York Times");
  command = ParseVoiceCommand("hey molt", context);
  EXPECT_EQ(command.heard, "");
}

TEST(VoiceCommandTest, CustomWakePhrase) {
  ParseContext context;
  context.wake_phrase = "computer";
  EXPECT_EQ(ParseVoiceCommand("computer open youtube", context).kind,
            VoiceCommandKind::kOpenSite);
  EXPECT_EQ(ParseVoiceCommand("hey computer louder", context).kind,
            VoiceCommandKind::kVolumeUp);
  // The default wake phrase is not taken out when the user chose another.
  EXPECT_EQ(ParseVoiceCommand("hey molt louder", context).kind,
            VoiceCommandKind::kUnknown);
}

TEST(VoiceCommandTest, RefusesRatherThanTruncates) {
  std::string long_text = "search for ";
  while (long_text.size() <= kMaxUtteranceBytes) long_text += "cats ";
  VoiceCommand command = ParseVoiceCommand(long_text, ParseContext());
  EXPECT_EQ(command.kind, VoiceCommandKind::kRefused);
  EXPECT_EQ(command.refusal, RefusalReason::kTooLong);
  EXPECT_TRUE(command.heard.empty());

  command = ParseVoiceCommand("open \xFF youtube", ParseContext());
  EXPECT_EQ(command.kind, VoiceCommandKind::kRefused);
  EXPECT_EQ(command.refusal, RefusalReason::kInvalidUtf8);
  command = ParseVoiceCommand("open \xED\xA0\x80", ParseContext());
  EXPECT_EQ(command.refusal, RefusalReason::kInvalidUtf8);

  // A query over the cap is the planner's, whole.
  std::string query = "search for ";
  while (query.size() <= kMaxCommandTextBytes + 20) query += "word ";
  command = ParseVoiceCommand(query, ParseContext());
  EXPECT_EQ(command.kind, VoiceCommandKind::kUnknown);
  EXPECT_FALSE(command.heard.empty());

  // Exactly at the cap is fine.
  std::string at_cap(kMaxUtteranceBytes, 'a');
  at_cap.replace(0, 5, "type ");
  command = ParseVoiceCommand(at_cap, ParseContext());
  EXPECT_NE(command.kind, VoiceCommandKind::kRefused);
}

TEST(VoiceCommandTest, LongInputIsQuick) {
  // The worst case the cap allows: every word a pattern word.
  std::string text;
  while (text.size() + 6 < kMaxUtteranceBytes) text += "go to ";
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 20; ++i) ParseVoiceCommand(text, ParseContext());
  const std::chrono::duration<double, std::milli> elapsed =
      std::chrono::steady_clock::now() - start;
  EXPECT_LT(elapsed.count() / 20, 200.0);
}

// --- The tables ---------------------------------------------------------------

TEST(VoiceCommandPhrasesTest, EveryPatternIsWellFormed) {
  for (const CommandPattern& pattern : CommandPatterns(Language::kEnglish)) {
    EXPECT_TRUE(IsValidCommandPattern(pattern.pattern)) << pattern.pattern;
  }
  for (std::string_view bad :
       {"", "(a|b", "[a]]", "{nope}", "{query} {text}", "{query} [a]",
        "({query})", "a||b", "(a|)", "{site?}", "UPPER", "a, b"}) {
    EXPECT_FALSE(IsValidCommandPattern(bad)) << bad;
  }
}

TEST(VoiceCommandPhrasesTest, EveryKindIsDocumentedAndItsExampleWorks) {
  std::set<VoiceCommandKind> documented;
  for (const CommandInfo& info : CommandInfos(Language::kEnglish)) {
    EXPECT_TRUE(documented.insert(info.kind).second)
        << VoiceCommandKindName(info.kind);
    EXPECT_FALSE(info.summary.empty());
    ParseContext context;
    context.expecting_yes_no = info.context == PatternContext::kYesNo;
    context.expecting_choice = info.context == PatternContext::kChoice;
    context.text_field_focused = info.context == PatternContext::kTextField;
    EXPECT_EQ(ParseVoiceCommand(info.example, context).kind, info.kind)
        << info.example;
  }
  for (size_t i = 0; i < kVoiceCommandKindCount; ++i) {
    const auto kind = static_cast<VoiceCommandKind>(i);
    if (kind == VoiceCommandKind::kUnknown ||
        kind == VoiceCommandKind::kRefused) {
      continue;
    }
    EXPECT_TRUE(documented.count(kind)) << VoiceCommandKindName(kind);
  }
  // Every documented kind has at least one pattern.
  std::set<VoiceCommandKind> patterned;
  for (const CommandPattern& pattern : CommandPatterns(Language::kEnglish)) {
    patterned.insert(pattern.kind);
  }
  for (VoiceCommandKind kind : documented) {
    EXPECT_TRUE(patterned.count(kind)) << VoiceCommandKindName(kind);
  }
}

TEST(VoiceCommandPhrasesTest, KindNamesAreUnique) {
  std::set<std::string_view> names;
  for (size_t i = 0; i < kVoiceCommandKindCount; ++i) {
    const std::string_view name =
        VoiceCommandKindName(static_cast<VoiceCommandKind>(i));
    EXPECT_FALSE(name.empty());
    EXPECT_TRUE(names.insert(name).second) << name;
  }
  EXPECT_EQ(VoiceCommandKindName(VoiceCommandKind::kOpenSite), "open_site");
  EXPECT_EQ(VoiceCommandKindName(VoiceCommandKind::kRunRoutine),
            "run_routine");
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
