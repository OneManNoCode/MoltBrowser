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
    kConfirmSites,  // confirmDraftSites: |sites|, after a real click
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
struct SaveRequest {
  SaveRequest();
  ~SaveRequest();
  SaveRequest(SaveRequest&&);
  SaveRequest& operator=(SaveRequest&&);

  automation::Script script;
  bool recorded_here = false;
};

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

// A new workflow's id: "wf_" and the time.
std::string NewWorkflowId();

}  // namespace agent_studio
}  // namespace molt_ai

#endif  // CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_AGENT_STORAGE_H_
