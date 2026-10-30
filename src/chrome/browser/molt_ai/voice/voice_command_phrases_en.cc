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
    {K::kListRoutines,
     "(what|which) (are|is) my routines"},
    {K::kListRoutines,
     "(what|which) routines (do i have|are there|have i got|have i saved)"},
    {K::kListRoutines,
     "(list|read|tell me|say) [all] [the|my] routines"},
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
    {K::kClearField,
     "(clear|erase|empty|delete|remove) [the|this] (field|box|text box|"
     "text|input|search box|entry)",
     C::kTextField},
    {K::kClearField, "(clear|erase|delete) [it|that|everything|all]",
     C::kTextField},
    {K::kReadBack, "read (it|that|this|what i (typed|wrote|said)) back",
     C::kTextField},
    {K::kReadBack, "read back [what i (typed|wrote)]", C::kTextField},
    {K::kReadBack, "(what did i (type|write)|read what i (typed|wrote))",
     C::kTextField},
    {K::kSubmit,
     "(submit|send|enter|go|search|done|send it|submit it|send the message)",
     C::kTextField},
    {K::kSubmit, "(press|hit) (enter|return)"},
    {K::kSubmit, "submit [the] form"},
    {K::kType, "(type|write|dictate) {text}"},

    // --- Scrolling (before "go back": "go back to the top") --------------
    {K::kScrollToTop,
     "(scroll|go|jump|move|go back) [up] to the (top|beginning|start) [of "
     "the page]"},
    {K::kScrollToTop, "(top of [the] page|page top|scroll top|back to top)"},
    {K::kScrollToBottom,
     "(scroll|go|jump|move) [down] to the (bottom|end) [of the page]"},
    {K::kScrollToBottom, "(bottom of [the] page|page bottom|scroll bottom)"},
    {K::kScrollDown, "page down", C::kAlways, V::kPageStep},
    {K::kScrollUp, "page up", C::kAlways, V::kPageStep},
    {K::kScrollDown, "(scroll|go|move) down {pages?}"},
    {K::kScrollDown, "(scroll|keep scrolling|scroll more|down) {pages?}"},
    {K::kScrollUp, "(scroll|go|move) up {pages?}"},
    {K::kScrollUp, "up {pages?}"},
    {K::kScrollLeft, "scroll left"},
    {K::kScrollRight, "scroll right"},

    // --- Seeking (before "go back" and "go forward") ----------------------
    {K::kRestart,
     "(restart|start over|replay|play (it|this|that) again|play again|watch "
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
    {K::kForward, "(go|navigate) forward [a page|one page]"},
    {K::kForward, "forward"},
    {K::kNextPage, "[go to] [the] next page [of results]"},
    {K::kNextPage, "(more results|show more results|load more results)"},
    {K::kReload, "(reload|refresh) [the|this] [page|tab|site|website|it]"},
    {K::kReload, "load [the|this] page again"},
    {K::kNewTab, "(open|new|start|create) [a] new tab"},
    {K::kNewTab, "(new tab|open [a] tab|open another tab)"},
    {K::kCloseTab, "close [this|the|current] tab"},
    {K::kCloseTab, "close tab {tab}"},
    {K::kCloseTab, "close [the] {tab} tab"},
    {K::kSwitchTab, "[go to|switch to|move to] [the] next tab", C::kAlways,
     V::kNextTab},
    {K::kSwitchTab, "[go to|switch to|move to] [the] previous tab", C::kAlways,
     V::kPreviousTab},
    {K::kSwitchTab, "(switch|go|change|move|jump) to tab {tab}"},
    {K::kSwitchTab, "tab {tab}"},
    {K::kSwitchTab, "(switch|go|change|move|jump) to [the] {tab} tab"},
    {K::kSwitchTab, "switch to {tab}"},

    // --- Viewing ------------------------------------------------------------
    {K::kZoomReset,
     "((reset|normal|default) zoom|zoom (reset|normal|to normal|back to "
     "normal)|(actual|normal|original|default) size|reset [the] zoom level)"},
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
    {K::kSkipAd,
     "skip [the|this|these] (ad|ads|advert|adverts|advertisement|"
     "advertisements|commercial)"},
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
    {K::kCaptionsOff,
     "(turn off|hide|disable|switch off|stop|remove|take off) [the] "
     "(captions|subtitles|closed captions|cc|subtitle)"},
    {K::kCaptionsOff,
     "((captions|subtitles|closed captions|cc) off|no "
     "(captions|subtitles))"},
    {K::kNormalSpeed,
     "((normal|regular|default|usual) [playback] speed|reset [the] "
     "[playback] speed|speed (normal|to normal|back to normal)|play at "
     "normal speed)"},
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
     "((whats|what is) on [the|my|this] (screen|page)|what do you see|what "
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

    // --- Choosing -----------------------------------------------------------
    {K::kShowNumbers,
     "(show [me] [the] (numbers|labels|number labels)|(numbers|labels) on|"
     "(number|label) everything|turn on [the] (numbers|labels))"},
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
    {K::kOpenSite,
     "(open|go to|take me to|visit|navigate to|launch|load|bring up|pull "
     "up) {site} in [a] new tab",
     C::kAlways, V::kNewTab},
    {K::kOpenSite,
     "(open|go to|take me to|visit|navigate to|launch|load|bring up|pull "
     "up) {site}"},
    {K::kChoose, "(open|go to|follow|visit|show me|show|bring up) {choice}",
     C::kAlways, V::kVerbOpen},
    {K::kFindInPage, "find [the word] {query} (on|in) (this|the) page"},
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

    // --- A choice on its own ------------------------------------------------
    {K::kChoose, "{number}", C::kChoice},
    {K::kChoose, "{position}"},
    {K::kChoose, "{pick}", C::kChoice},
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
    {K::kRestart, "Media", "Play from the beginning.", "start over"},
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
    {K::kUndo, "Talking to the browser", "Undo the last action.", "undo"},
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
