// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/translate/page_translate_messages.h"

#include <iterator>

#include "chrome/browser/molt_ai/web_ai/web_ai_translation.h"

namespace molt_ai::page_translate {

namespace {

constexpr char kOnDevice[] =
    "Translation runs on this device: the page's text isn't sent anywhere.";
constexpr char kMoltNetNote[] =
    " The site can see the translation, and so the language you read.";
constexpr char kNeedsModel[] =
    "Translation runs on this device and needs an AI model. Nothing is "
    "downloaded until you choose one.";
constexpr char kNoDevice[] =
    "This device can't run on-device AI, so pages can't be translated here.";
constexpr char kChooseModel[] = "Choose a model";
constexpr char kStop[] = "Stop";

Footnote Text(std::string text) {
  Footnote footnote;
  footnote.text = std::move(text);
  return footnote;
}

Footnote WithLink(std::string text, FootnoteLink link) {
  Footnote footnote = Text(std::move(text));
  footnote.link = link;
  footnote.link_text = link == FootnoteLink::kStop ? kStop : kChooseModel;
  return footnote;
}

Footnote Idle(const StatusSnapshot& status) {
  if (!status.device_supported) {
    return Text(kNoDevice);
  }
  if (status.model_installed == false) {
    return WithLink(kNeedsModel, FootnoteLink::kChooseModel);
  }
  return Text(std::string(kOnDevice) + (status.moltnet ? kMoltNetNote : ""));
}

Footnote Done(const StatusSnapshot& status) {
  switch (status.report.problem) {
    case Problem::kNothingToTranslate:
      return Text("There's no text on this page to translate.");
    case Problem::kStopped:
      return Text(
          "Stopped. What was translated stays until you show the original.");
    case Problem::kTimedOut:
      return Text(
          "Translation took too long and stopped. What was translated "
          "stays.");
    case Problem::kModelErrors:
      return Text(
          "The model stopped giving usable translations, so part of the "
          "page wasn't translated.");
    case Problem::kPageUnreadable:
      return Text(
          "The page changed while it was translated, so part of it wasn't "
          "translated.");
    default:
      break;
  }
  if (status.truncated) {
    return Text(
        "Translated on this device. The page is long, so only its first "
        "part was translated.");
  }
  if (status.failed_units > 0) {
    return Text(
        "Translated on this device. Some text couldn't be translated and "
        "was left as it was.");
  }
  return Text("Translated on this device. Machine translation can be wrong.");
}

Footnote Failed(const StatusSnapshot& status) {
  switch (status.report.problem) {
    case Problem::kNoModel:
      return WithLink(kNeedsModel, FootnoteLink::kChooseModel);
    case Problem::kDeviceUnsupported:
      return Text(kNoDevice);
    case Problem::kModelFailed:
      return WithLink(
          "The AI model couldn't be loaded. A smaller model may fit this "
          "device.",
          FootnoteLink::kChooseModel);
    case Problem::kUnknownSource:
      return Text(
          "MoltBrowser couldn't tell this page's language. Choose it under "
          "Options, then translate.");
    case Problem::kUnsupportedPair:
      return Text("On-device translation covers " +
                  std::to_string(std::size(web_ai::kTranslatorLanguages)) +
                  " languages, and this page's language or the one you "
                  "chose isn't one of them.");
    case Problem::kIdentical:
      return Text("This page is already in that language.");
    case Problem::kPageUnreadable:
      return Text("The page's text couldn't be read.");
    case Problem::kModelErrors:
      return Text("The model couldn't translate this page.");
    case Problem::kStopped:
      return Text("Stopped before anything was translated.");
    case Problem::kTimedOut:
      return Text("Translation took too long and stopped.");
    case Problem::kNone:
    case Problem::kNothingToTranslate:
      break;
  }
  return Text("This page couldn't be translated.");
}

}  // namespace

Footnote FootnoteFor(const StatusSnapshot& status) {
  switch (status.phase) {
    case Phase::kIdle:
      return Idle(status);
    case Phase::kPreparing:
      return WithLink("Getting ready to translate on this device...",
                      FootnoteLink::kStop);
    case Phase::kTranslating:
      return WithLink("Translating on this device... " +
                          std::to_string(status.settled) + " of " +
                          std::to_string(status.total),
                      FootnoteLink::kStop);
    case Phase::kDone:
      return Done(status);
    case Phase::kFailed:
      return Failed(status);
  }
  return Text(kOnDevice);
}

}  // namespace molt_ai::page_translate
