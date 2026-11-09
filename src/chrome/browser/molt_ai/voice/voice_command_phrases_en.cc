// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// English: the direct-command patterns (voice_command_phrases.h has the
// syntax) and the command reference. Order matters: the first pattern
// that takes the whole utterance wins.

#include <iterator>
#include <vector>

#include "chrome/browser/molt_ai/voice/voice_command.h"
#include "chrome/browser/molt_ai/voice/voice_command_phrases.h"

namespace molt_ai {
namespace voice {

namespace {

using K = VoiceCommandKind;
using C = PatternContext;
using V = PatternVariant;

constexpr CommandPattern kPatterns[] = {
    // --- Ways out: always first -------------------------------------------
    {K::kCancel,
     "(cancel|never mind|nevermind|forget it|forget that|forget about it|"
     "cancel that|abort|scratch that|stop that request)"},
    {K::kStopSpeaking, "(stop|quit) (talking|speaking|reading|reading aloud)"},
    {K::kStopSpeaking, "(shut up|be quiet|quiet|silence|hush|enough)"},
    {K::kStopLoading, "stop loading [the|this] [page|site|tab]"},
    {K::kStopLoading, "stop the page [from] loading"},
    {K::kPause,
     "stop (the video|the music|the song|playback|playing|the movie|the "
     "audio)"},
    {K::kStop, "stop [it|that|everything|all|now]"},

    // --- Voice mode itself ------------------------------------------------
    {K::kVoiceModeOff,
     "(turn|switch|shut) (off|down) voice [mode|control|commands]"},
    {K::kVoiceModeOff, "(turn|switch) voice [mode|control] off"},
    {K::kVoiceModeOff, "voice [mode|control] off"},
    {K::kVoiceModeOff,
     "(stop|quit|exit|end|leave|close) voice [mode|control|commands]"},
    {K::kVoiceModeOff, "stop listening [to me]"},
    {K::kVoiceModeOn, "(turn|switch) on voice [mode|control|commands]"},
    {K::kVoiceModeOn, "(turn|switch) voice [mode|control] on"},
    {K::kVoiceModeOn, "voice [mode|control] on"},
    {K::kVoiceModeOn, "(start|enter) voice [mode|control|commands]"},
    {K::kVoiceModeOn, "start listening [to me]"},

    // --- Corrections and answers ----------------------------------------
    {K::kCorrection, "[no|nope|wrong] i (said|meant|asked for) {text}"},
    {K::kCorrection,
     "(no|nope) (not that|thats wrong|wrong) i (said|meant) {text}"},
    {K::kCorrection, "i didnt say that i said {text}"},
    {K::kYes,
     "(yes|yeah|yep|yup|ya|sure|correct|confirm|confirmed|affirmative|"
     "absolutely|of course|okay|ok|alright|all right|right|do it|go ahead|"
     "go for it|thats right|thats correct|that is right|please do) [do it|go "
     "ahead|thats right|thats it|i do|it is|i am|confirm|please do]",
     C::kYesNo},
    {K::kNo,
     "(no|nope|nah|negative|wrong|incorrect|no way) [dont|dont do it|do not|"
     "i dont|its not|it isnt|it is not|not that one|cancel]",
     C::kYesNo},
    {K::kNo, "(dont|do not) [do it|do that]", C::kYesNo},
    // "No" heard as "now" (the spoken suite's file run), only as the answer
    // to a yes-or-no question: no is the answer that does nothing.
    {K::kNo, "now", C::kYesNo},
    {K::kUndo,
     "undo [that|it|this|the last (thing|action|step|change)|what you did]"},
    {K::kRepeat,
     "(repeat|repeat that|repeat it|say again|say it again|say that again|"
     "come again|pardon|pardon me|what did you say|once more|sorry what|"
     "what was that)"},
    {K::kHelp,
     "(help|help me|i need help|what can i say|what can i do|what do i say|"
     "what can you do|what commands are there|show commands|list commands|"
     "commands|show me what i can say|how does this work)"},
    {K::kWhereAmI,
     "(where am i|where are we|what (page|site|website|tab) (is this|am i "
     "on|are we on)|what is this (page|site|website)|whats this (page|site|"
     "website))"},

    // --- Speech -----------------------------------------------------------
    {K::kSpeakFaster, "(speak|talk|read) (faster|quicker|more quickly)"},
    {K::kSpeakFaster, "speed up (your voice|your speech|speech|talking)"},
    {K::kSpeakSlower, "(speak|talk|read) (slower|more slowly|slowly)"},
    {K::kSpeakSlower, "slow down (your voice|your speech|speech|talking)"},
    {K::kVerbosity,
     "(be brief|brief mode|brief|short answers|shorter answers|keep it "
     "short|verbosity brief|brief verbosity)",
     C::kAlways, V::kBrief},
    {K::kVerbosity,
     "(normal verbosity|verbosity normal|normal detail|default verbosity)",
     C::kAlways, V::kNormal},
    {K::kVerbosity,
     "(full verbosity|verbosity full|full detail|be verbose|verbose mode|"
     "tell me everything)",
     C::kAlways, V::kFull},
    {K::kVerbosity, "(more detail|more details|say more|more verbose)",
     C::kAlways, V::kMore},
    {K::kVerbosity, "(less detail|fewer details|say less|less verbose)",
     C::kAlways, V::kLess},

    // --- The rest of the browser (voice_browser_logic.h) ------------------
    // Before sites and choices: "open my bookmarks" is no site called "my
    // bookmarks", and "open the last download" no item of the page.
    {K::kStartOver,
     "(start over|start again|begin again|lets start over|lets start again|"
     "start from scratch|start afresh|reset|reset everything|clear "
     "the conversation|new conversation)"},
    // "Start of": "over" heard short.
    {K::kStartOver, "start of"},
    {K::kNextMatch,
     "[go to|jump to|show|show me] [the] (next|following) (match|occurrence|"
     "hit|one i found)"},
    {K::kNextMatch, "find (next|again|the next one)"},
    {K::kPreviousMatch,
     "[go to|jump to|show|show me] [the] previous (match|occurrence|hit)"},
    {K::kPreviousMatch, "find (previous|the previous one)"},
    // Bookmarks.
    {K::kRemoveBookmark,
     "(remove|delete) (this|the|that) bookmark [for this page|of this page]"},
    {K::kRemoveBookmark,
     "(remove|delete|take) (this|it|this page|the page|this site) (from|out "
     "of) [my|the] bookmarks"},
    {K::kRemoveBookmark, "unbookmark [this|it|this page|the page|this site]"},
    // "Bookmarked this page": how the bundled English recognizer hears
    // "bookmark this page" in some voices.
    {K::kBookmarkPage,
     "(bookmark|bookmarked) (this|it|this page|the page|this site|this "
     "tab|that)"},
    {K::kBookmarkPage,
     "(add|save|put) (this|it|this page|the page|this site|this tab) (to|in|"
     "as) [a|my|the] (bookmarks|bookmark)"},
    {K::kBookmarkPage,
     "(add|make|create) a bookmark [for this page|for this site|here]"},
    {K::kListBookmarks,
     "(open|show|read|list|tell me) [me] [all] my bookmarks"},
    {K::kListBookmarks,
     "(open|show) [me] [the] (bookmarks|bookmark manager|bookmarks page)"},
    {K::kListBookmarks, "(what are|whats) my bookmarks"},
    {K::kListBookmarks, "my bookmarks"},
    {K::kOpenBookmark,
     "(open|go to|load) [the|my] bookmark [called|named|for] {name}"},
    // "Open the book, Mark Trams return to Lyon": "bookmark" heard as two
    // words (the spoken suite's runs, every voice).
    {K::kOpenBookmark,
     "(open|go to|load) [the|my] book mark [called|named|for] {name}"},
    {K::kOpenBookmark, "(open|go to|load) {name} (from|in) [my|the] bookmarks"},
    // History.
    // "Closed tip", "close tab", "Reuben": how the bundled English
    // recognizer hears "reopen the closed tab" in some voices.
    {K::kReopenTab,
     "(reopen|restore|bring back|undo close|reuben) [the] [last] "
     "[closed|close] (tab|tip)"},
    {K::kReopenTab,
     "(reopen|restore|bring back|open again) the [last] tab i [just] closed"},
    {K::kReopenTab, "open [the] [last] (closed|close) (tab|tip)"},
    {K::kListHistory,
     "(open|show|read|list|tell me) [me] my [browsing] history"},
    {K::kListHistory, "(open|show) [me] [the] [browsing] history [page]"},
    {K::kListHistory,
     "(what|which) (pages|sites) did i (visit|open|look at) [recently]"},
    {K::kListHistory, "my history"},
    // "Show" lost into the wake phrase ("He mold to my history").
    {K::kListHistory, "to my history"},
    {K::kOpenFromHistory,
     "(open|go to|load|find) {query} (from|in) [my|the] history"},
    // Downloads.
    {K::kOpenDownload,
     "open [the] (last|latest|newest|most recent|last downloaded|latest "
     "downloaded) (download|file|downloaded file)",
     C::kAlways, V::kNewest},
    {K::kOpenDownload, "open [the] download [number] {number}"},
    {K::kOpenDownload, "open [the] {position} (download|downloaded file)"},
    {K::kListDownloads,
     "(open|show|read|list|tell me) [me] [all] my downloads"},
    {K::kListDownloads, "(open|show) [me] [the] downloads [page|folder]"},
    {K::kListDownloads, "what (did i|have i) download [recently]"},
    {K::kListDownloads, "my downloads"},
    // "Show" heard as "to" and "draw" (the spoken suite's file run).
    {K::kListDownloads, "(to|draw|sure) my downloads"},
    // Settings. Those that weaken a protection are only changed by a click.
    {K::kProtectedSetting,
     "[turn on|switch on|enable|always] say [the] [confirmation] word (aloud|"
     "out loud) [always|anyway]"},
    {K::kProtectedSetting,
     "(turn on|switch on|enable) [the] (say the word aloud|confirmation word "
     "aloud)"},
    {K::kProtectedSetting,
     "(trust|approve|allow) (this|the|that|my) (workflow|routine|automation)"
     " [on (all|every) (site|sites)]",
     C::kAlways, V::kWorkflowTrust},
    {K::kProtectedSetting,
     "(change|raise) the trust of (this|the|that|my) (workflow|routine|"
     "automation)",
     C::kAlways, V::kWorkflowTrust},
    {K::kAlwaysListening,
     "(turn|switch) on (always listening|always listen|listening all the "
     "time)"},
    {K::kAlwaysListening, "(turn|switch) always listening on"},
    {K::kAlwaysListening,
     "(always listening on|always listen [to me]|listen all the time)"},
    {K::kAlwaysListening,
     "(turn|switch) off (always listening|listening all the time)", C::kAlways,
     V::kOff},
    {K::kAlwaysListening, "(turn|switch) always listening off", C::kAlways,
     V::kOff},
    {K::kAlwaysListening,
     "(always listening off|stop always listening|stop listening all the "
     "time|only listen after hey molt)",
     C::kAlways, V::kOff},
    {K::kOpenSettings,
     "(open|show|go to) [me] [the|my] (privacy|security|privacy and "
     "security) settings",
     C::kAlways, V::kSettingsPrivacy},
    {K::kOpenSettings,
     "(open|show|go to) [me] [the] (ai|molt ai|assistant) settings", C::kAlways,
     V::kSettingsAI},
    {K::kOpenSettings,
     "(open|show|go to) [me] [the] (voice mode|voice|voice control) settings",
     C::kAlways, V::kSettingsVoice},
    // "Mold net": how the bundled English recognizer hears "MoltNet".
    {K::kOpenSettings,
     "(open|show|go to) [me] [the] (moltnet|molt net|mold net|tor) settings",
     C::kAlways, V::kSettingsMoltNet},
    {K::kOpenSettings,
     "(open|show|go to) [me] [the] [browser|browsers|molt browser|"
     "moltbrowser] settings"},
    {K::kOpenSettings, "(browser|browsers) settings"},
    // Windows: voice mode stays in the window it is in.
    {K::kNewWindow,
     "(open|new|start|create) [a] [new] (private|incognito) window", C::kAlways,
     V::kWindowPrivate},
    {K::kNewWindow, "(private|incognito) window", C::kAlways,
     V::kWindowPrivate},
    // "Moldenet", "moldenate", "mold the": more of how it hears "MoltNet".
    {K::kNewWindow,
     "(open|new|start|create) [a] [new] (moltnet|molt net|mold net|moldnet|"
     "moldenet|moldenate|mold the|tor) window",
     C::kAlways, V::kWindowMoltNet},
    {K::kNewWindow, "(moltnet|molt net|mold net|moldnet|moldenet) window",
     C::kAlways, V::kWindowMoltNet},
    {K::kNewWindow, "(open|start|create) [a] new window"},
    {K::kNewWindow, "(new window|open [a] window|open another window)"},
    // "Windjo", "winjo", "windel": "window" as some voices' is heard (the
    // spoken suite's recognizer check), and "molten at" for "MoltNet".
    {K::kNewWindow, "[open] [a] new (windjo|winjo|windel)"},
    {K::kNewWindow, "[open] [a] new (private|incognito) (windjo|winjo|windel)",
     C::kAlways, V::kWindowPrivate},
    {K::kNewWindow,
     "[open] [a] new (molten at|molten|moldnet|moltnet) (window|windjo|winjo)",
     C::kAlways, V::kWindowMoltNet},
    {K::kCloseWindow, "close [this|the|current] window"},
    {K::kSwitchWindow,
     "(switch|go|move|change|jump) to [the] (other|next|previous|last) window"},
    {K::kSwitchWindow, "(other|next) window"},
    // The AI chat. "The eye" and "a chat": how the bundled English
    // recognizer hears "the AI" and "AI chat" (neither does more than ask
    // the on-device model or open its panel).
    {K::kAskAI,
     "ask (the ai|ai|the assistant|the ai assistant|molt ai|the chat|the ai "
     "chat|the eye|the a chat) [about|whether|if] {query}"},
    // "As the AI": "ask" heard without its k (asking only answers).
    {K::kAskAI, "as (the ai|the assistant|the ai assistant) {query}"},
    {K::kOpenAIChat,
     "(open|show|start) [me] [the] (ai chat|ai|chat|assistant|ai assistant|"
     "ai panel|chat panel|a chat|achat|eye chat|ai checked)"},
    // The browser's own buttons and menus.
    {K::kShowBrowserButtons,
     "(show|read|list|tell me) [me] [the] (browser|browsers|toolbar|"
     "toolbars|browser toolbar) (buttons|controls)"},
    {K::kShowBrowserButtons,
     "(what are|whats on) the (browser|browsers|toolbar) (buttons|controls|"
     "toolbar)"},
    {K::kShowBrowserButtons,
     "(browser buttons|browsers buttons|toolbar "
     "buttons|show [me] the toolbar)"},
    {K::kCloseMenu,
     "(close|exit|leave|hide|dismiss) [the] [app|main|browser] "
     "menu"},
    {K::kOpenMenu,
     "(open|show) [me] [the] (app|main|browser|browsers|chrome|moltbrowser) "
     "menu"},
    {K::kOpenMenu, "open the menu"},
    {K::kOpenMenu, "(app menu|browser menu|main menu|browsers menu)"},

    // --- Routines (learning) ----------------------------------------------
    // What was learned, forgotten: before "forget {name}", which would take
    // these as a routine's name.
    {K::kForgetLearning,
     "(forget|delete|erase) (everything|all) [that] you (learned|learnt|"
     "know) [about me|from me]"},
    {K::kForgetLearning,
     "(forget|delete|erase) (everything|all) [that] youve (learned|learnt)"
     " [about me|from me]"},
    {K::kForgetLearning,
     "(forget|delete|erase) (everything|all) [that] you have (learned|"
     "learnt) [about me|from me]"},
    {K::kForgetWord,
     "(forget|delete|remove) the (word|name|phrase|correction) {name}"},
    {K::kListRoutines, "(what|which) (are|is) my routines"},
    {K::kListRoutines,
     "(what|which) routines (do i have|are there|have i got|have i saved)"},
    {K::kListRoutines, "(list|read|tell me|say) [all] [the|my] routines"},
    {K::kListRoutines, "(my routines|routines)"},
    {K::kRememberRoutine,
     "(remember|save) (this|that|it) as [a routine|routine] [called|named] "
     "{name}"},
    {K::kRememberRoutine, "save this routine as {name}"},
    {K::kForgetRoutine,
     "(forget|delete|remove) [the|my] routine [called|named] {name}"},
    {K::kForgetRoutine, "forget {name}"},
    {K::kRunRoutine,
     "(run|start|do|play) [the|my] routine [called|named] {name}"},
    {K::kRunRoutine, "run {name}"},

    // --- Dictation --------------------------------------------------------
    {K::kNewLine, "(new line|newline|next line|line break|new paragraph)",
     C::kTextField},
    {K::kDeleteLastWord,
     "(delete|remove|erase|scratch|undo) [the] (last|previous) word",
     C::kTextField},
    {K::kDeleteLastWord, "delete that word", C::kTextField},
    // "Last" heard as "list" (the spoken suite's live runs), and "delete"
    // as "to leap" (its headphones runs).
    {K::kDeleteLastWord, "(delete|remove|erase) the list word", C::kTextField},
    {K::kDeleteLastWord, "to leap the (last|list) word", C::kTextField},
    {K::kClearField,
     "(clear|erase|empty|delete|remove) [the|this] (field|box|text box|"
     "text|input|search box|entry)",
     C::kTextField},
    {K::kClearField, "(clear|erase|delete) [it|that|everything|all]",
     C::kTextField},
    // "Back" heard as "Beck" (the spoken suite's file run).
    {K::kReadBack, "read (it|that|this|what i (typed|wrote|said)) (back|beck)",
     C::kTextField},
    {K::kReadBack, "read back [what i (typed|wrote)]", C::kTextField},
    {K::kReadBack, "(what did i (type|write)|read what i (typed|wrote))",
     C::kTextField},
    {K::kSubmit,
     "(submit|send|enter|go|search|done|send it|submit it|send the message)",
     C::kTextField},
    {K::kSubmit, "(press|hit) (enter|return)"},
    {K::kSubmit, "submit [the] form"},
    // "Tight": how some voices' "type" is heard.
    {K::kType, "(type|write|dictate|tight) {text}"},

    // --- Scrolling (before "go back": "go back to the top") --------------
    // "Roll", "girl": "scroll" with its start lost to the wake phrase
    // before it ("Hey Molt. Sc|roll to the top"), as the spoken suite's
    // runs heard it.
    {K::kScrollToTop,
     "(scroll|roll|go|jump|move|go back) [up] to the (top|beginning|start) "
     "[of the page]"},
    // "Top" and "bottom" heard as "tar" and "borrow" (the spoken suite's
    // recognizer check).
    {K::kScrollToTop, "(scroll|roll) [up] to the tar"},
    {K::kScrollToTop, "(top of [the] page|page top|scroll top|back to top)"},
    {K::kScrollToBottom,
     "(scroll|roll|girl|go|jump|move) [down] to the (bottom|end) [of the "
     "page]"},
    {K::kScrollToBottom, "(bottom of [the] page|page bottom|scroll bottom)"},
    {K::kScrollToBottom, "(scroll|roll) [down] to the borrow"},
    {K::kScrollDown, "page down", C::kAlways, V::kPageStep},
    {K::kScrollUp, "page up", C::kAlways, V::kPageStep},
    {K::kScrollDown, "(scroll|go|move) down {pages?}"},
    {K::kScrollDown, "(scroll|keep scrolling|scroll more|down) {pages?}"},
    {K::kScrollUp, "(scroll|go|move) up {pages?}"},
    {K::kScrollUp, "up {pages?}"},
    {K::kScrollLeft, "scroll left"},
    {K::kScrollRight, "scroll right"},

    // --- Seeking (before "go back" and "go forward") ----------------------
    // "Start over" is kStartOver (above): while something plays, it plays
    // from the start too.
    {K::kRestart,
     "(restart|replay|play (it|this|that) again|play again|watch "
     "again|from the (beginning|start|top)|start from the (beginning|start)|"
     "play from the (beginning|start)|restart the video|rewind to the "
     "(beginning|start))"},
    {K::kSkipBack,
     "(go|skip|jump|move|seek) (back|backward|backwards) [by] {seconds}"},
    {K::kSkipBack,
     "(skip|jump|seek) (back|backward|backwards) [by] {seconds?}"},
    {K::kSkipBack, "rewind [by] {seconds?}"},
    {K::kSkipBack, "back {seconds}"},
    {K::kSkipForward,
     "(go|skip|jump|move|seek) (ahead|forward|forwards) [by] {seconds}"},
    {K::kSkipForward,
     "(skip|jump|seek) (ahead|forward|forwards) [by] {seconds?}"},
    {K::kSkipForward, "fast forward [by] {seconds?}"},
    {K::kSkipForward, "(skip|jump) [by] {seconds?}"},
    {K::kSkipForward, "forward {seconds}"},

    // --- Going places -------------------------------------------------------
    {K::kHome,
     "(go home|take me home|home|home page|homepage|go to [the] home page|"
     "open [the] home page)"},
    {K::kBack,
     "(go|navigate|take me) back [a page|one page|to the (previous|last) "
     "page]"},
    {K::kBack, "(back|previous page|last page|go to the previous page)"},
    // "Go back" heard as "feel back" (the spoken suite's file run).
    {K::kBack, "feel back"},
    {K::kForward, "(go|navigate) forward [a page|one page]"},
    {K::kForward, "forward"},
    // "Go forward" heard as "feel forward", as "go back" is "feel back".
    {K::kForward, "feel forward"},
    {K::kNextPage, "[go to] [the] next page [of results]"},
    {K::kNextPage, "(more results|show more results|load more results)"},
    {K::kReload, "(reload|refresh) [the|this] [page|tab|site|website|it]"},
    {K::kReload, "load [the|this] page again"},
    // "Reload" heard as "roulette" (the spoken suite's file run), "Roledge"
    // and "Rolete" (its microphone runs).
    {K::kReload, "(roulette|roledge|rolete)"},
    {K::kNewTab, "(open|new|start|create) [a] new tab"},
    // "Open a new tip": "tab" heard as "tip", before a site of that name.
    {K::kNewTab, "(open|start|create) [a] new tip"},
    {K::kNewTab, "(new tab|open [a] tab|open another tab)"},
    {K::kCloseTab, "close [this|the|current] tab"},
    // "Tab" heard as "ted" (the spoken suite's file run).
    {K::kCloseTab, "close (this|the) ted"},
    {K::kCloseTab, "close tab {tab}"},
    {K::kCloseTab, "close [the] {tab} tab"},
    {K::kSwitchTab, "[go to|switch to|move to] [the] next tab", C::kAlways,
     V::kNextTab},
    {K::kSwitchTab, "[go to|switch to|move to] [the] previous tab", C::kAlways,
     V::kPreviousTab},
    // "Tab" heard as "tomb" (the spoken suite's file run).
    {K::kSwitchTab, "[go to|switch to|move to] [the] previous tomb",
     C::kAlways, V::kPreviousTab},
    {K::kSwitchTab, "(switch|go|change|move|jump) to (tab|tip) {tab}"},
    {K::kSwitchTab, "tab {tab}"},
    {K::kSwitchTab, "(switch|go|change|move|jump) to [the] {tab} tab"},
    {K::kSwitchTab, "switch to {tab}"},

    // --- Viewing ------------------------------------------------------------
    {K::kZoomReset,
     "((reset|rosetta|bosetta|for setta|normal|default) zoom|zoom (reset|"
     "normal|to normal|back to normal)|(actual|normal|original|default) "
     "size|reset [the] zoom level)"},
    // "Reset zoom" run together ("Resetzum", "Rosetzum").
    {K::kZoomReset, "(resetzum|rosetzum|resetzoom)"},
    {K::kZoomIn,
     "(zoom in|zoom in more|magnify|enlarge|increase [the] zoom|(bigger|"
     "larger) [text]|make (it|the text|text|everything|the page) (bigger|"
     "larger))"},
    {K::kZoomOut,
     "(zoom out|zoom out more|shrink|decrease [the] zoom|smaller [text]|"
     "make (it|the text|text|everything|the page) smaller)"},

    // --- Media --------------------------------------------------------------
    {K::kPause,
     "(pause|hold on) [it|this|that|the video|the music|the song|playback|"
     "the movie|the audio|reading|the reading]"},
    // "Pores", "paws": "pause" as it sounds without an r, how the bundled
    // English recognizer hears it from a British voice (the spoken suite's
    // runs, file and live microphone).
    {K::kPause, "(pores|paws) [it|this|that|the video|the music|the song]"},
    {K::kSkipAd,
     "skip [the|this|these] (ad|ads|advert|adverts|advertisement|"
     "advertisements|commercial)"},
    // "Ad" heard as "ed" (the spoken suite's file run).
    {K::kSkipAd, "skip (the|this) ed"},
    {K::kResume,
     "(resume|unpause|continue|keep playing|continue playing|carry on) "
     "[it|playback|the video|the music|the song|playing]"},
    {K::kNextTrack,
     "[play|skip to|go to] [the] next (video|song|track|episode|clip)"},
    {K::kNextTrack, "skip (this|the) (video|song|track|episode|clip)"},
    {K::kPreviousTrack,
     "[play|go to|go back to] [the] previous (video|song|track|episode|"
     "clip)"},
    {K::kFullScreen,
     "[go|enter|make it|switch to|turn on|go to] (full screen|fullscreen) "
     "[mode]"},
    {K::kFullScreen, "maximize [the video|it|the player]"},
    {K::kExitFullScreen,
     "(exit|leave|close|quit|end|turn off|get out of|cancel) (full screen|"
     "fullscreen) [mode]"},
    {K::kExitFullScreen,
     "((small|normal|regular) screen|minimize [the video|it|the player])"},
    {K::kCaptionsOn,
     "(turn on|show|enable|switch on|start|put on) [the] (captions|"
     "subtitles|closed captions|cc|subtitle)"},
    {K::kCaptionsOn, "(captions|subtitles|closed captions|cc) on"},
    // "Captions" heard as "catchens" (the spoken suite's file run).
    {K::kCaptionsOn, "turn on [the] catchens"},
    {K::kCaptionsOff,
     "(turn off|hide|disable|switch off|stop|remove|take off) [the] "
     "(captions|subtitles|closed captions|cc|subtitle)"},
    {K::kCaptionsOff,
     "((captions|subtitles|closed captions|cc) off|no "
     "(captions|subtitles))"},
    // "Captions of": "off" heard short, or run into the "s" before it
    // ("caption's wolf", "caption's both").
    {K::kCaptionsOff, "(captions|subtitles) (of|wolf|both)"},
    {K::kNormalSpeed,
     "((normal|regular|default|usual) [playback] speed|reset [the] "
     "[playback] speed|speed (normal|to normal|back to normal)|play at "
     "normal speed)"},
    {K::kSetSpeed,
     "[set|change|put|turn] [the] [playback|video] speed [to|at] {speed}"},
    {K::kSetSpeed, "(play|watch|listen|go) at {speed} [speed]"},
    {K::kSetSpeed, "(play|watch|listen) [it] {speed} (faster|speed)"},
    {K::kSetSpeed, "{speed} speed"},
    {K::kSpeedUp,
     "(speed up|faster|play faster|go faster|speed it up|increase [the] "
     "[playback] speed)"},
    {K::kSlowDown,
     "(slow down|slower|play slower|go slower|slow it down|(decrease|reduce) "
     "[the] [playback] speed)"},
    {K::kVolumeUp,
     "(louder|volume up|turn it up|turn up the volume|turn the volume up|"
     "turn up|more volume|make it louder|crank it up|pump it up|(increase|"
     "raise) [the] volume|a (bit|little) louder|much louder)"},
    {K::kVolumeDown,
     "(quieter|softer|volume down|turn it down|turn down the volume|turn the "
     "volume down|turn down|less volume|make it quieter|lower|(decrease|"
     "lower|reduce) [the] volume|a (bit|little) quieter|much quieter)"},
    {K::kSetVolume, "[set|turn|change|put] [the] volume [to|at] {level}"},
    {K::kSetVolume, "{level} volume"},
    // "Set" heard as "said" (the spoken suite's file run).
    {K::kSetVolume, "said [the] volume (to|at) {level}"},
    {K::kMute,
     "mute [it|this|everything|the (sound|audio|video|tab|volume|music)]"},
    {K::kMute,
     "((sound|audio) off|turn (off|down) the sound|turn the sound off)"},
    {K::kUnmute,
     "unmute [it|this|everything|the (sound|audio|video|tab|volume|music)]"},
    {K::kUnmute,
     "((sound|audio) on|turn on the sound|turn the sound [back] on)"},
    {K::kPlay,
     "(play|hit play|press play|start playing|start playback) [it|this|"
     "that|the video|the song|the music|the media|the movie]"},

    // --- Reading ------------------------------------------------------------
    {K::kReadSelection,
     "read [the|my] (selection|selected text|highlighted text|highlighted "
     "part|highlight)"},
    {K::kReadSelection, "read (what is|whats|what i) (selected|highlighted)"},
    {K::kReadTitle,
     "(read|say|tell me) the (title|page title|title of the page|name of "
     "the page|page name)"},
    // "Title" heard as "tile" (the spoken suite's file run).
    {K::kReadTitle, "read the tile"},
    {K::kReadTitle,
     "(what is|whats) the (title|page title|title of the page|name of "
     "(this|the) page)"},
    {K::kReadHeadings,
     "(read|list|say|tell me|what are) [the|all] [the] (headings|section "
     "headings|sections|subheadings)"},
    {K::kReadHeadings, "headings"},
    {K::kReadLinks, "(read|list|say|tell me|what are) [the|all] [the] links"},
    {K::kReadResults,
     "(read|list|say|tell me|what are) [the|all] [the] (results|search "
     "results|options|choices|items|list|videos|stories|articles|products) "
     "[again]"},
    {K::kReadResults, "what are my (options|choices)"},
    {K::kReadFromHere,
     "(read from here|read on|read the rest [of the page]|read from this "
     "point|(continue|keep|resume|carry on) reading)"},
    {K::kReadPage,
     "read [the|this] [whole|entire] (page|article|story|text|post|"
     "content) [aloud|out loud|to me]"},
    {K::kReadPage,
     "(read|start reading|begin reading) [it|this|everything|all|"
     "everything on the page] [aloud|out loud|to me]"},
    {K::kDescribeScreen,
     "((whats|what is) on [the|my|this|a] (screen|page)|what do you see|what "
     "can you see|describe [the|this] (screen|page|view)|(whats|what is) "
     "here)"},
    {K::kNextHeading,
     "[go to|jump to|skip to|move to|read] [the] next (heading|section|"
     "header)"},
    {K::kPreviousHeading,
     "[go to|jump to|skip to|move to|read] [the] previous (heading|section|"
     "header)"},
    {K::kNextLink, "[go to|jump to|move to|read] [the] next link"},
    {K::kPreviousLink, "[go to|jump to|move to|read] [the] previous link"},
    {K::kNextParagraph, "[go to|jump to|move to|read] [the] next paragraph"},
    {K::kPreviousParagraph,
     "[go to|jump to|move to|read] [the] previous paragraph"},
    {K::kNextItem,
     "[go to|jump to|move to|read] [the] next (item|result|entry|element)"},
    {K::kNextItem, "next"},
    {K::kPreviousItem,
     "[go to|jump to|move to|read] [the] previous (item|result|entry|"
     "element)"},
    {K::kPreviousItem, "previous"},
    {K::kReadSection,
     "(read|read me|go to|jump to|skip to|move to) [the] (section|part) (on|"
     "about|called|named|titled) {query}"},
    {K::kReadSection,
     "(read|read me|go to|jump to|skip to|move to) [the] {query} (section|"
     "part)"},

    // --- Questions about the page (page_questions.h) ----------------------
    // Before choosing, which would take "what is <a description>". The
    // questions that are commands ("what's on the screen", "where am I",
    // "what's the title") are above and stay commands; a question none of
    // these takes still goes to the planner.
    {K::kAskPage,
     "(tell me more|what else does (it|the page|this page|the "
     "article|this article) say)",
     C::kAlways, V::kAskMore},
    {K::kAskPage,
     "(read|tell|give|list|say) [me|us] the (main|key|important|most "
     "important) (points|ideas|facts|takeaways|things)",
     C::kAlways, V::kAskMainPoints},
    {K::kAskPage,
     "(what are|whats|what is) the (main|key|important|most important) "
     "(points|ideas|facts|takeaways|point|idea)",
     C::kAlways, V::kAskMainPoints},
    {K::kAskPage, "(main points|key points|key takeaways|takeaways)",
     C::kAlways, V::kAskMainPoints},
    {K::kAskPage,
     "what (does|do|did) (it|this|that|they|the page|this page|the article|"
     "this article|the story|this story|the post|this post|the text|the "
     "site|this site) (say|tell me|tell us|mention|write) (about|on|"
     "regarding) {query}",
     C::kAlways, V::kAskTopic},
    {K::kAskPage,
     "(does|do|did) (it|this|the page|this page|the article|this article|the "
     "story|this story|the post|this post|the text) (say|mention|tell me) "
     "(anything|something) (about|on|regarding) {query}",
     C::kAlways, V::kAskTopic},
    {K::kAskPage,
     "(does|do) (it|the page|this page|the article|this article|the text) "
     "(mention|talk about|cover|discuss) {query}",
     C::kAlways, V::kAskTopic},
    {K::kAskPage,
     "(tell me|read me) what (it|this|the page|this page|the article|this "
     "article) says (about|on|regarding) {query}",
     C::kAlways, V::kAskTopic},
    {K::kAskPage,
     "(what is|whats) (this|the|this whole|the whole) (page|article|story|"
     "post|site|website|text|document|video|piece|essay|thread) about",
     C::kAlways, V::kAskAbout},
    {K::kAskPage, "(what is|whats) (it|this|that) [all] about", C::kAlways,
     V::kAskAbout},
    {K::kAskPage,
     "what (does|do) (it|this|the page|this page|the article|this article|"
     "the story|this story|the post|this post|the text) say",
     C::kAlways, V::kAskAbout},
    {K::kAskPage,
     "(summarize|summarise|sum up|recap) [it|this|that|the (page|article|"
     "story|post|text|document|video)|this (page|article|story|post|text|"
     "document|video)]",
     C::kAlways, V::kAskAbout},
    {K::kAskPage,
     "(give me|read me|tell me) [a|the] (summary|short summary|quick "
     "summary|gist|overview) [of (it|this|that|the (page|article|story|"
     "post|text)|this (page|article|story|post|text))]",
     C::kAlways, V::kAskAbout},
    {K::kAskPage,
     "(whats|what is) the (gist|summary) [of (it|this|that|the (page|"
     "article|story|post|text)|this (page|article|story|post|text))]",
     C::kAlways, V::kAskAbout},
    {K::kAskPage, "(summary|tldr|tl dr)", C::kAlways, V::kAskAbout},
    {K::kAskPage,
     "who (wrote|authored|published|posted) (this|it|that|the (page|article|"
     "story|post|piece)|this (page|article|story|post|piece))",
     C::kAlways, V::kAskQuestion},
    {K::kAskPage,
     "(who is|whos) the (author|writer|publisher) [of (this|it|the (page|"
     "article|story|post|piece)|this (page|article|story|post|piece))]",
     C::kAlways, V::kAskQuestion},
    {K::kAskPage, "who is (this|it) (by|from|written by)", C::kAlways,
     V::kAskQuestion},
    {K::kAskPage,
     "(what is|whats) the (price|cost) [of (it|this|this one|that|that one|"
     "the (product|item|thing)|this (product|item|thing))]",
     C::kAlways, V::kAskQuestion},
    {K::kAskPage,
     "how much (is|does|do) (it|this|this one|that|that one|the (product|"
     "item)|this (product|item)) [cost|costs]",
     C::kAlways, V::kAskQuestion},
    {K::kAskPage, "what does (it|this|that) cost", C::kAlways, V::kAskQuestion},
    {K::kAskPage,
     "when was (this|it|that|the (page|article|story|post|piece)|this (page|"
     "article|story|post|piece)) (written|published|posted|updated|last "
     "updated|released)",
     C::kAlways, V::kAskQuestion},
    {K::kAskPage,
     "(ask the page|ask this page|ask the article|ask this article) {query}",
     C::kAlways, V::kAskQuestion},
    {K::kAskPage,
     "according to (this|the) (page|article|story|post|site) {query}",
     C::kAlways, V::kAskQuestion},

    // --- Choosing -----------------------------------------------------------
    {K::kShowNumbers,
     "((show|joe) [me] [the] (numbers|labels|number labels)|(numbers|labels) "
     "on|(number|label) everything|turn on [the] (numbers|labels))"},
    {K::kHideNumbers,
     "(hide [the] (numbers|labels)|(numbers|labels) off|(remove|clear) "
     "[the] (numbers|labels)|turn off [the] (numbers|labels)|no numbers)"},
    // A choice by number or position wins over a site of the same words.
    {K::kChoose, "(play|watch|listen to) {choice}", C::kAlways, V::kVerbPlay},
    {K::kChoose, "(open|go to|follow|visit|show me|show|bring up) {position}",
     C::kAlways, V::kVerbOpen},
    {K::kChoose,
     "(click|click on|press|tap|tap on|push|hit|activate|check|uncheck|"
     "tick|untick|toggle) {choice}",
     C::kAlways, V::kVerbClick},
    {K::kChoose, "(select|choose|pick|focus|focus on) {choice}", C::kAlways,
     V::kVerbSelect},
    {K::kChoose, "read {choice}", C::kAlways, V::kVerbRead},
    {K::kChoose,
     "(describe|tell me about|tell me more about|what is|whats|what about|"
     "more about|more info on|details of|details on) {choice}",
     C::kAlways, V::kVerbDescribe},
    // Right after a list, a description beats a site name.
    {K::kChoose, "(open|go to|follow|visit|show me|show|bring up) {choice}",
     C::kChoice, V::kVerbOpen},

    // --- Sites and searches -------------------------------------------------
    // "And a new tab": "in a new tab" as the recognizer hears it (the
    // spoken suite's live run).
    {K::kOpenSite,
     "(open|go to|take me to|visit|navigate to|launch|load|bring up|pull "
     "up) {site} (in|and) [a] new tab",
     C::kAlways, V::kNewTab},
    {K::kOpenSite,
     "(open|go to|take me to|visit|navigate to|launch|load|bring up|pull "
     "up) {site}"},
    // "Open YouTube" heard as "Hoping YouTube" (the spoken suite's file run);
    // only a site's name follows.
    {K::kOpenSite, "hoping {site}"},
    {K::kChoose, "(open|go to|follow|visit|show me|show|bring up) {choice}",
     C::kAlways, V::kVerbOpen},
    {K::kFindInPage, "find [the word] {query} (on|in) (this|the) page"},
    // "Fine timetable on this page": "find" heard as "fine".
    {K::kFindInPage, "fine [the word] {query} (on|in) (this|the) page"},
    {K::kFindInPage, "(find on page|find in page|find on this page) {query}"},
    {K::kFindInPage, "search (this|the) page for {query}"},
    {K::kSearch, "search (the web|the internet|online|the net) for {query}",
     C::kAlways, V::kScopeDefaultEngine},
    {K::kSearch, "(web search|internet search|search online for) {query}",
     C::kAlways, V::kScopeDefaultEngine},
    {K::kSearch, "search (this site|this website|here) for {query}", C::kAlways,
     V::kScopeCurrentSite},
    {K::kSearch, "search for {query} (on this site|on this website|here)",
     C::kAlways, V::kScopeCurrentSite},
    {K::kSearch, "search {site} for {query}", C::kAlways, V::kScopeNamedSite},
    {K::kSearch, "(search for|search|look up|look for) {query} (on|in) {site}",
     C::kAlways, V::kScopeNamedSite},
    {K::kSearch, "(search for|search|look up|look for) {query}"},
    // An address said on its own: "example dot com".
    {K::kOpenSite, "{address}"},
    // "Take", "tech": more of how "type" is heard (the spoken suite's runs,
    // "take hello there"), only into a focused text field, and after the
    // sites and searches, so that "take me to ...", "take me home" and
    // "take me back" mean themselves there too.
    {K::kType, "(take|tech) {text}", C::kTextField},

    // --- A choice on its own ------------------------------------------------
    {K::kChoose, "{number}", C::kChoice},
    {K::kChoose, "{position}"},
    {K::kChoose, "{pick}", C::kChoice},

    // --- The answer to "Which site would you like me to open?" ------------
    // Last, so that every command still means itself.
    {K::kCancel, "(no|no thanks|no thank you|nothing|none|never mind|not now)",
     C::kSite},
    {K::kOpenSite, "{site}", C::kSite},
};

constexpr CommandInfo kInfos[] = {
    // Going places.
    {K::kOpenSite, "Going places", "Open a site by its name or its address.",
     "open YouTube"},
    {K::kSearch, "Going places",
     "Search: on the site you are on (or the search engine when it has no "
     "search box), the web, or a site you name.",
     "search for Bohemian Rhapsody"},
    {K::kFindInPage, "Going places", "Find words on this page.",
     "find opening hours on this page"},
    {K::kNextMatch, "Going places", "The next place the words were found.",
     "next match"},
    {K::kPreviousMatch, "Going places",
     "The place before, where the words were found.", "previous match"},
    {K::kBack, "Going places", "Go back to the previous page.", "go back"},
    {K::kForward, "Going places", "Go forward again.", "go forward"},
    {K::kReload, "Going places", "Load the page again.", "reload"},
    {K::kStopLoading, "Going places", "Stop loading the page.", "stop loading"},
    {K::kHome, "Going places", "Go to your home page.", "go home"},
    {K::kNextPage, "Going places", "The next page of results.", "next page"},
    {K::kNewTab, "Going places", "Open a new tab.", "open a new tab"},
    {K::kCloseTab, "Going places",
     "Close this tab, or a tab by number or name.", "close this tab"},
    {K::kSwitchTab, "Going places",
     "Switch tabs: by number, position or name, or the next one.",
     "switch to tab 2"},
    // Viewing.
    {K::kScrollDown, "Viewing",
     "Scroll down: a bit, half a page, a page, or several.", "scroll down"},
    {K::kScrollUp, "Viewing", "Scroll up.", "scroll up a page"},
    {K::kScrollLeft, "Viewing", "Scroll left.", "scroll left"},
    {K::kScrollRight, "Viewing", "Scroll right.", "scroll right"},
    {K::kScrollToTop, "Viewing", "Go to the top of the page.",
     "scroll to the top"},
    {K::kScrollToBottom, "Viewing", "Go to the bottom of the page.",
     "scroll to the bottom"},
    {K::kZoomIn, "Viewing", "Make everything bigger.", "zoom in"},
    {K::kZoomOut, "Viewing", "Make everything smaller.", "zoom out"},
    {K::kZoomReset, "Viewing", "Back to the normal size.", "reset zoom"},
    // Media.
    {K::kPlay, "Media", "Play.", "play"},
    {K::kPause, "Media", "Pause.", "pause"},
    {K::kResume, "Media", "Carry on playing.", "resume"},
    {K::kVolumeUp, "Media", "Louder.", "louder"},
    {K::kVolumeDown, "Media", "Quieter.", "quieter"},
    {K::kSetVolume, "Media",
     "Set the volume: 0 to 10, a percentage, half or max.",
     "set the volume to 5"},
    {K::kMute, "Media", "Mute the sound.", "mute"},
    {K::kUnmute, "Media", "Turn the sound back on.", "unmute"},
    {K::kSkipForward, "Media", "Skip ahead, by a step or by how long you say.",
     "skip ahead one minute"},
    {K::kSkipBack, "Media", "Go back in the video.", "go back 10 seconds"},
    {K::kRestart, "Media", "Play from the beginning.",
     "play from the beginning"},
    {K::kNextTrack, "Media", "The next video or song.", "next video"},
    {K::kPreviousTrack, "Media", "The previous video or song.",
     "previous song"},
    {K::kFullScreen, "Media", "Full screen.", "full screen"},
    {K::kExitFullScreen, "Media", "Leave full screen.", "exit full screen"},
    {K::kCaptionsOn, "Media", "Show captions.", "turn on captions"},
    {K::kCaptionsOff, "Media", "Hide captions.", "captions off"},
    {K::kSpeedUp, "Media", "Play faster.", "speed up"},
    {K::kSlowDown, "Media", "Play slower.", "slow down"},
    {K::kNormalSpeed, "Media", "Back to normal speed.", "normal speed"},
    {K::kSetSpeed, "Media",
     "Set the playback speed: 1.5, one and a half, double, half, from a "
     "quarter to 4 times.",
     "speed 1.5"},
    {K::kSkipAd, "Media", "Skip the ad, once the player lets you.",
     "skip the ad"},
    // Reading.
    {K::kReadPage, "Reading", "Read the page aloud.", "read the page"},
    {K::kReadFromHere, "Reading", "Read on from where you are.",
     "read from here"},
    {K::kReadHeadings, "Reading", "Read the headings.", "read the headings"},
    {K::kReadLinks, "Reading", "Read the links.", "read the links"},
    {K::kReadResults, "Reading", "Read the list of results again.",
     "read the results"},
    {K::kReadTitle, "Reading", "Read the page's title.", "read the title"},
    {K::kReadSelection, "Reading", "Read the selected text.",
     "read the selection"},
    {K::kDescribeScreen, "Reading", "Describe what is on the screen.",
     "what's on the screen"},
    {K::kNextHeading, "Reading", "Move to the next heading.", "next heading"},
    {K::kPreviousHeading, "Reading", "Move to the previous heading.",
     "previous heading"},
    {K::kNextLink, "Reading", "Move to the next link.", "next link"},
    {K::kPreviousLink, "Reading", "Move to the previous link.",
     "previous link"},
    {K::kNextItem, "Reading", "Move to the next item of the list.",
     "next item"},
    {K::kPreviousItem, "Reading", "Move to the previous item.",
     "previous item"},
    {K::kNextParagraph, "Reading", "Read the next paragraph.",
     "next paragraph"},
    {K::kPreviousParagraph, "Reading", "Read the paragraph before.",
     "previous paragraph"},
    {K::kReadSection, "Reading",
     "Read one section of the page, by its heading.",
     "read the section on history"},
    // Questions about the page.
    {K::kAskPage, "Questions about the page",
     "Ask about the page you are on: what it is about, its main points, "
     "what it says about something, who wrote it, the price. The on-device "
     "AI answers from the page's text, briefly, and says when the page "
     "doesn't say. \"Tell me more\" goes on with the next passages.",
     "what is this page about"},
    // Choosing.
    {K::kChoose, "Choosing",
     "Choose from what is on screen: by number, by position, or by name, "
     "with what to do (open, play, click, select, read, describe).",
     "play the fourth one"},
    {K::kShowNumbers, "Choosing", "Put numbers on everything you can choose.",
     "show numbers"},
    {K::kHideNumbers, "Choosing", "Take the numbers away.", "hide numbers"},
    // Typing.
    {K::kType, "Typing", "Type into the selected field.", "type hello world"},
    {K::kNewLine, "Typing", "Start a new line.", "new line", C::kTextField},
    {K::kDeleteLastWord, "Typing", "Delete the last word.",
     "delete the last word", C::kTextField},
    {K::kClearField, "Typing", "Clear the field.", "clear the field",
     C::kTextField},
    {K::kReadBack, "Typing", "Read back what you typed.", "read it back",
     C::kTextField},
    {K::kSubmit, "Typing", "Send what you typed.", "submit", C::kTextField},
    // Conversation.
    {K::kYes, "Talking to the browser", "Answer a question: yes.", "yes",
     C::kYesNo},
    {K::kNo, "Talking to the browser", "Answer a question: no.", "no",
     C::kYesNo},
    {K::kCancel, "Talking to the browser", "Cancel what you asked for.",
     "cancel"},
    {K::kStop, "Talking to the browser",
     "Stop: speaking, and whatever is running.", "stop"},
    {K::kStopSpeaking, "Talking to the browser", "Stop speaking.",
     "stop talking"},
    {K::kUndo, "Talking to the browser",
     "Undo what voice mode did last: reopen the tab it closed, put back what "
     "it typed, go back from a page it opened, or put back a bookmark.",
     "undo"},
    {K::kStartOver, "Talking to the browser",
     "Start over: forget the list, the question and what was being done, "
     "and listen again. While something plays, play it from the start.",
     "start over"},
    {K::kRepeat, "Talking to the browser", "Say that again.", "repeat"},
    {K::kHelp, "Talking to the browser", "Hear what you can say.",
     "what can I say"},
    {K::kWhereAmI, "Talking to the browser", "Hear where you are.",
     "where am I"},
    {K::kSpeakFaster, "Talking to the browser", "Speak faster.",
     "speak faster"},
    {K::kSpeakSlower, "Talking to the browser", "Speak slower.",
     "speak slower"},
    {K::kVerbosity, "Talking to the browser",
     "How much the browser says: brief, normal or full, more or less.",
     "be brief"},
    {K::kCorrection, "Talking to the browser", "Correct what was heard.",
     "no, I said YouTube"},
    // The browser itself.
    {K::kBookmarkPage, "The browser itself", "Bookmark this page.",
     "bookmark this page"},
    {K::kRemoveBookmark, "The browser itself", "Remove this page's bookmark.",
     "remove this bookmark"},
    {K::kListBookmarks, "The browser itself",
     "Hear your bookmarks, to open one by number or name.",
     "open my bookmarks"},
    {K::kOpenBookmark, "The browser itself", "Open a bookmark by its name.",
     "open the bookmark news"},
    {K::kListHistory, "The browser itself",
     "Hear the pages you visited, to open one by number or name.",
     "show my history"},
    {K::kReopenTab, "The browser itself", "Reopen the tab you closed last.",
     "reopen the closed tab"},
    {K::kOpenFromHistory, "The browser itself",
     "Open a page from your history by its site or title.",
     "open the weather from my history"},
    {K::kListDownloads, "The browser itself",
     "Hear your downloads, newest first, to open one.", "show my downloads"},
    {K::kOpenDownload, "The browser itself",
     "Open a downloaded file: the last one, or by its number. A file that "
     "can run programs isn't opened by voice.",
     "open the last download"},
    {K::kOpenSettings, "The browser itself",
     "Open settings: the browser's, or privacy, AI, voice mode or MoltNet "
     "settings.",
     "open settings"},
    {K::kAlwaysListening, "The browser itself",
     "Always listening on (after your yes) or off: every sentence is a "
     "command, without Hey Molt.",
     "turn on always listening"},
    {K::kProtectedSetting, "The browser itself",
     "Settings that weaken a protection (saying the confirmation word aloud, "
     "a workflow's trust) are changed with a click in settings only: the "
     "browser says where.",
     "say the word aloud"},
    {K::kNewWindow, "The browser itself",
     "Open a new window, a private window or a MoltNet window. Voice mode "
     "stays in this window, and never listens in MoltNet.",
     "new window"},
    {K::kCloseWindow, "The browser itself",
     "Close this window, after your yes; voice mode moves to another "
     "window.",
     "close this window"},
    {K::kSwitchWindow, "The browser itself",
     "Switch to your other window; voice mode comes with you.",
     "switch to the other window"},
    {K::kAskAI, "The browser itself",
     "Ask the on-device AI a question. Its answer is read to you, and "
     "nothing is done because of it.",
     "ask the AI how far away is the moon"},
    {K::kOpenAIChat, "The browser itself", "Open the AI chat beside the page.",
     "open the AI chat"},
    {K::kShowBrowserButtons, "The browser itself",
     "Hear the browser's own buttons (the toolbar, the tabs, the bookmarks "
     "bar, the side panel), to press one by number or name.",
     "show the browser's buttons"},
    {K::kOpenMenu, "The browser itself",
     "Open the browser's menu, then say what to click.", "open the menu"},
    {K::kCloseMenu, "The browser itself", "Close the browser's menu.",
     "close the menu"},
    // Routines.
    {K::kRememberRoutine, "Routines", "Save what you just did as a routine.",
     "remember this as my news"},
    {K::kRunRoutine, "Routines", "Run a routine by its name.", "run my news"},
    {K::kForgetRoutine, "Routines", "Delete a routine.", "forget my news"},
    {K::kListRoutines, "Routines", "Hear your routines.",
     "what are my routines"},
    {K::kForgetWord, "Routines",
     "Forget a word or a correction the browser learned from you.",
     "forget the word Queen"},
    {K::kForgetLearning, "Routines",
     "Forget everything the browser learned from you: routines, words, "
     "corrections and site hints.",
     "forget everything you learned"},
    // Voice mode itself.
    {K::kVoiceModeOn, "Voice mode",
     "Start listening (typed in the Agent mode panel).", "turn on voice mode"},
    {K::kVoiceModeOff, "Voice mode",
     "Stop listening. The browser asks first: afterwards the Voice button "
     "in the toolbar, or settings, turns it back on.",
     "turn off voice mode"},
};

}  // namespace

std::vector<CommandPattern> MakeEnglishCommandPatterns() {
  return std::vector<CommandPattern>(std::begin(kPatterns),
                                     std::end(kPatterns));
}

std::vector<CommandInfo> MakeEnglishCommandInfos() {
  return std::vector<CommandInfo>(std::begin(kInfos), std::end(kInfos));
}

}  // namespace voice
}  // namespace molt_ai
