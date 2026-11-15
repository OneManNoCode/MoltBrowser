// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The Agent studio's (molt_ai_agent_ui.cc) reads and changes of workflows
// and routines, off the UI thread (round 44). Each storage directory has
// one sequence, and the studio does everything there that touches it:
//   - the profile's routines (<profile>/MoltVoiceRoutines/): the profile's
//     VoiceLearning sequence (VoiceLearning::routines_task_runner()), which
//     voice mode's own saves, claims and deletions use, so the studio and
//     voice mode never interleave a read-modify-write of a routine (a
//     routine's name made unique and saved, say);
//   - the shared storage (~/.moltbrowser/automations/):
//     AutomationStorage::SharedSequence(). The callers still on the UI
//     thread (the runner, the scheduler, molt://ai-automation) change it
//     through AutomationStorage's lock of the directory, which the studio's
//     changes hold too (Update()).
// A message about a workflow is answered in at most two steps, where it
// is looked for: AnswerInRoutines() on the profile's sequence (one of its
// routines taught by voice), then, unless that answered,
// AnswerInSharedStorage() on the shared one (an ordinary workflow; a voice
// routine there is from before profiles, kept here first). A save goes to
// the storage it belongs in, after a look at the other one. Listing reads
// each storage on its sequence. The page's messages and answers are the
// same as when all this ran on the UI thread.
//
// A workflow whose file was changed outside the browser
// (SecurityPolicy::changed_outside: the shared storage loads it as an
// unconfirmed draft, automation_storage.h) is shown for review like a
// draft from an agent run: it neither runs, nor is scheduled or trusted,
// until confirmDraftSites, after a real click, with the review token of
// exactly what the page showed (docs/design/AGENT_ACTIONS.md §3.6), its
// schedule starting over. A save that names a workflow (one the page
// opened) is an edit of what is stored then, refused if it is gone; only a
// save that names none is a new workflow (SaveRequest, KindOfSave()). An
// interval schedule the studio writes without a last fire (saveSchedule,
// confirmDraftSites) counts from then, and saveSchedule and
// confirmDraftSites answer with the trigger they saved.
//
// Blocking (file I/O); base::Value and the storages only, so
// molt_ai_unittests tests it on temporary directories.

#ifndef CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_AGENT_STORAGE_H_
#define CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_AGENT_STORAGE_H_

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/voice/voice_routine_store.h"

namespace molt_ai {
namespace agent_studio {

// Where the studio's workflows are.
struct StudioPlaces {
  // The profile's routines (voice::VoiceRoutinesDir()); empty without them
  // (off the record, MoltNet's windows included).
  base::FilePath routines_dir;
  // The shared storage's root for tests (VoiceLearning::shared_root()),
  // where the routines from before profiles are; empty: ~/.moltbrowser.
  base::FilePath shared_root;
  // The shared storage's directory; empty when the studio has none (no
  // automation service, as in browser tests).
  base::FilePath shared_dir;
};

// A message's answer, built where the storage is.
struct Answer {
  Answer();
  ~Answer();
  Answer(Answer&&);
  Answer& operator=(Answer&&);

  // What the page gets.
  base::DictValue out;
  // The profile's routines changed: voice mode lists them again.
  bool routines_changed = false;
  // runWorkflow: the workflow to run.
  std::optional<automation::Script> script;
  // saveSchedule: the trigger saved.
  std::optional<automation::Trigger> trigger;
};

// A message about one workflow (its id), and what it carries.
struct WorkflowRequest {
  enum class Kind {
    kGet,           // getWorkflow
    kRuns,          // getRuns
    kDelete,        // deleteWorkflow
    kDuplicate,     // duplicateWorkflow: |name|
    kCheckRun,      // runWorkflow: the workflow, if it may run here
    kSchedule,      // saveSchedule: |trigger|
    kTrust,         // promoteTrust: |level|, after a real click
    kConfirmSites,  // confirmDraftSites: |sites| and |review_token|, after
                    // a real click
  };

  WorkflowRequest();
  ~WorkflowRequest();
  WorkflowRequest(const WorkflowRequest&);
  WorkflowRequest& operator=(const WorkflowRequest&);

  Kind kind = Kind::kGet;
  std::string id;
  std::string name;
  std::string level;
  std::vector<std::string> sites;
  // confirmDraftSites: the reviewToken of what the page showed (getWorkflow
  // or saveWorkflow): the draft is confirmed only while it is still that.
  std::string review_token;
  automation::Trigger trigger;
};

// On the profile's routines' sequence: the answer when |request|'s
// workflow is one of the profile's routines, else nullopt.
std::optional<Answer> AnswerInRoutines(const StudioPlaces& places,
                                       const WorkflowRequest& request);
// On the shared storage's sequence: the answer for its workflow (not a
// voice routine), or the message's "not found".
Answer AnswerInSharedStorage(const StudioPlaces& places,
                             const WorkflowRequest& request);

// saveWorkflow: |script| from the page (its id given or made, its creation
// time set). |recorded_here|: its id was made by stopRecording in this
// panel and this is its first save (UI state, used up by the caller).
// |new_workflow|: the page sent no id (a blank or imported workflow, or a
// recording), so it is saved under the id made for it or a free one made
// from it, never over another file. An id the page sent is of a workflow
// it read: an edit, refused when that workflow is gone or unreadable by
// the time it is saved (never saved as a new one). KindOfSave() decides.
struct SaveRequest {
  SaveRequest();
  ~SaveRequest();
  SaveRequest(SaveRequest&&);
  SaveRequest& operator=(SaveRequest&&);

  automation::Script script;
  bool recorded_here = false;
  bool new_workflow = false;
};

// saveWorkflow's kind, from the page's message: its "id" (|sent_id|) is of
// a workflow it read, so an edit, even when it is one of |minted| (the ids
// stopRecording made in this panel, not saved yet: a file put there since
// by anything else is changed outside like any other). With none, a new
// workflow: a recording's first save when its "recordedId"
// (|recorded_id|) is one of |minted| (the caller uses it up), else under
// an id the caller makes.
struct SaveKind {
  // The id to save under; empty: one the caller makes.
  std::string id;
  bool new_workflow = false;
  bool recorded_here = false;
};
SaveKind KindOfSave(const std::string& sent_id,
                    const std::string& recorded_id,
                    const std::set<std::string>& minted);

// What a storage holds under an id, for a save to the other one.
enum class Holds {
  kNothing,
  kRoutine,   // a voice routine (in the shared storage: one from
              // before profiles)
  kOrdinary,  // an ordinary workflow
};

// Step 1 of a save, on the sequence of the storage it doesn't go to: what
// the profile's routines (a voice routine only) or the shared storage hold
// under |id|.
Holds RoutinesHold(const StudioPlaces& places, const std::string& id);
Holds SharedStorageHolds(const StudioPlaces& places, const std::string& id);
// Step 2, on the sequence of the storage it goes to: a routine taught by
// voice (it has voice steps) to the profile's routines, an ordinary
// workflow to the shared storage. Refusals are the page's "error".
Answer SaveRoutine(const StudioPlaces& places,
                   SaveRequest request,
                   Holds in_shared_storage);
Answer SaveSharedWorkflow(const StudioPlaces& places,
                          SaveRequest request,
                          Holds in_routines);

// listWorkflows, on each storage's sequence: the profile's routines (each
// known by its file's name) and the routines from before profiles (claims
// cut short settled first: voice::VoiceRoutineStore::Unclaimed()); the
// shared storage's ordinary workflows.
struct RoutinesListing {
  RoutinesListing();
  ~RoutinesListing();
  RoutinesListing(RoutinesListing&&);
  RoutinesListing& operator=(RoutinesListing&&);

  std::vector<automation::Script> routines;
  std::vector<voice::RoutineSummary> unclaimed;
};
RoutinesListing ListRoutines(const StudioPlaces& places);
std::vector<automation::Script> ListSharedWorkflows(const StudioPlaces& places);

// The run's poll, on the shared storage's sequence: its workflow as
// stored.
std::optional<automation::Script> LoadSharedWorkflow(const StudioPlaces& places,
                                                     const std::string& id);

// stopRecording, on the shared storage's sequence: |id|, or it with "_"
// added until neither |minted| (ids this panel made) nor the shared
// storage has it.
std::string UnusedWorkflowId(const StudioPlaces& places,
                             std::string id,
                             const std::set<std::string>& minted);

// The sites an unconfirmed draft (agent_run_draft.h) asks the user to
// confirm: the hosts its steps target, as the browser computes them.
base::ListValue DraftSites(const automation::Script& script);

// An unconfirmed draft's review token, which confirmDraftSites must bring
// back: AutomationSecurity::SecurityDigest() of what the page was shown
// (64 lowercase hex digits). A draft that changed since (its file edited
// outside the browser, its steps saved) isn't confirmed with it.
std::string ReviewToken(const automation::Script& script);

// A new workflow's id: "wf_" and the time.
std::string NewWorkflowId();

}  // namespace agent_studio
}  // namespace molt_ai

#endif  // CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_AGENT_STORAGE_H_
