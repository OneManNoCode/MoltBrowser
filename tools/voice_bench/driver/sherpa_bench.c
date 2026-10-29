// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// sherpa_bench: drives sherpa-onnx through its C API (the API a MoltBrowser
// speech service would use) and prints one JSON object per line.
//
//   sherpa_bench info
//   sherpa_bench offline --list L --threads N <model flags>
//                        [--vad silero_vad.onnx]  (decode only what the VAD
//                         finds, plus 0.25 s each side, as deployed)
//   sherpa_bench stream  --list L --threads N <model flags>
//                        [--vad silero_vad.onnx] [--vad-silence 0.5]
//                        [--chunk-ms 100] [--tail-pad-ms 300]
//                        [--decoding greedy_search|
//                         modified_beam_search] [--hotwords-score 1.5]
//   sherpa_bench kws     --list L --threads N <model flags> --keywords F
//                        [--kws-threshold 0.25] [--kws-score 1.0]
//   sherpa_bench tts     --list L --threads N <tts flags>
//   sherpa_bench listen  --list L --threads N --listen-engine kws|stream
//                        <model flags> [--vad F] [--keywords F]
//                        (real time, with sleeps: CPU used while listening)
//
// List files are UTF-8, one item per line, fields separated by TABs:
//   offline, kws:  id  wav
//   stream:        id  wav  speech_start_s  speech_end_s  [hotwords]
//                  (hotwords: phrases separated by '/', as sherpa-onnx's
//                   CreateOnlineStreamWithHotwords takes them)
//   tts:           id  sid  speed  text  [out.wav]
//
// Streaming runs are simulated in real time without sleeping: the audio is
// fed in chunks as a microphone would deliver them, the compute time of each
// chunk is measured, and a chunk's result is "available" at
//   done[k] = max(audio_time[k], done[k-1]) + compute[k],
// so the reported latencies are what a live stream on this machine would see
// (provided nothing else competes for the CPU).
//
// Model flags (paths): --encoder --decoder --joiner --tokens --model-type
//   --modeling-unit --bpe-vocab --preprocessor --uncached-decoder
//   --cached-decoder --merged-decoder --kind
// --kind (offline): transducer | nemo_transducer | moonshine | moonshine2 |
//   whisper
// TTS flags: --kind kokoro|vits|matcha --model --voices --tokens --lexicon
//   --vocoder
// --provider-config FILE passes ONNX Runtime session settings through
//   sherpa-onnx's "cpu:FILE" provider string (e.g.
//   SessionConfig.session.intra_op.allow_spinning=0).
//
// Memory: "rss_after_load_kb" is the resident set right after the model is
// created, "peak_rss_kb" the process peak at exit.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif
#endif

#include "sherpa-onnx/c-api/c-api.h"

// ---------------------------------------------------------------- utilities

static double NowMs(void) {
#if defined(_WIN32)
  static LARGE_INTEGER freq;
  LARGE_INTEGER now;
  if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&now);
  return (double)now.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

static long CurrentRssKb(void) {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS pmc;
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
    return (long)(pmc.WorkingSetSize / 1024);
  return -1;
#elif defined(__APPLE__)
  struct mach_task_basic_info info;
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info,
                &count) == KERN_SUCCESS)
    return (long)(info.resident_size / 1024);
  return -1;
#else
  FILE *f = fopen("/proc/self/statm", "r");
  long pages = -1, resident = -1;
  if (!f) return -1;
  if (fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = -1;
  fclose(f);
  return resident < 0 ? -1 : resident * (sysconf(_SC_PAGESIZE) / 1024);
#endif
}

static long PeakRssKb(void) {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS pmc;
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
    return (long)(pmc.PeakWorkingSetSize / 1024);
  return -1;
#else
  struct rusage ru;
  if (getrusage(RUSAGE_SELF, &ru) != 0) return -1;
#if defined(__APPLE__)
  return (long)(ru.ru_maxrss / 1024);  // bytes on macOS
#else
  return (long)ru.ru_maxrss;  // kilobytes on Linux
#endif
#endif
}

static double CpuSeconds(void) {
#if defined(_WIN32)
  FILETIME c, e, k, u;
  if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return -1;
  ULARGE_INTEGER kk, uu;
  kk.LowPart = k.dwLowDateTime;
  kk.HighPart = k.dwHighDateTime;
  uu.LowPart = u.dwLowDateTime;
  uu.HighPart = u.dwHighDateTime;
  return (double)(kk.QuadPart + uu.QuadPart) / 1e7;
#else
  struct rusage ru;
  if (getrusage(RUSAGE_SELF, &ru) != 0) return -1;
  return (double)ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 +
         (double)ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
#endif
}

static void SleepUntilMs(double t) {
  double now = NowMs();
  if (t <= now) return;
#if defined(_WIN32)
  Sleep((DWORD)(t - now));
#else
  struct timespec ts;
  double d = (t - now) / 1000.0;
  ts.tv_sec = (time_t)d;
  ts.tv_nsec = (long)((d - (double)ts.tv_sec) * 1e9);
  nanosleep(&ts, NULL);
#endif
}

static void PrintJsonString(const char *s) {
  putchar('"');
  for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; ++p) {
    switch (*p) {
      case '"': fputs("\\\"", stdout); break;
      case '\\': fputs("\\\\", stdout); break;
      case '\n': fputs("\\n", stdout); break;
      case '\r': fputs("\\r", stdout); break;
      case '\t': fputs("\\t", stdout); break;
      default:
        if (*p < 0x20) printf("\\u%04x", *p);
        else putchar(*p);
    }
  }
  putchar('"');
}

static void Die(const char *msg, const char *arg) {
  fprintf(stderr, "sherpa_bench: %s%s%s\n", msg, arg ? ": " : "",
          arg ? arg : "");
  exit(2);
}

// ---------------------------------------------------------------- options

typedef struct Options {
  const char *mode;
  const char *list;
  int threads;
  const char *kind;
  const char *encoder, *decoder, *joiner, *tokens, *model_type;
  const char *modeling_unit, *bpe_vocab;
  const char *preprocessor, *uncached_decoder, *cached_decoder,
      *merged_decoder;
  const char *vad;
  float vad_silence;
  float vad_threshold;
  int chunk_ms;
  int tail_pad_ms;
  const char *decoding;
  float hotwords_score;
  const char *keywords;
  float kws_threshold, kws_score;
  int kws_trailing_blanks;
  const char *model, *voices, *lexicon, *vocoder;
  int tokens_from_buffer;
  const char *provider;
  const char *listen_engine;
} Options;

static const char *Need(int argc, char **argv, int *i) {
  if (*i + 1 >= argc) Die("missing value for", argv[*i]);
  return argv[++*i];
}

static void ParseOptions(int argc, char **argv, Options *o) {
  memset(o, 0, sizeof(*o));
  o->threads = 1;
  o->vad_silence = 0.5f;
  o->vad_threshold = 0.5f;
  o->chunk_ms = 100;
  o->tail_pad_ms = 300;
  o->decoding = "greedy_search";
  o->hotwords_score = 1.5f;
  o->kws_threshold = 0.25f;
  o->kws_score = 1.0f;
  o->kws_trailing_blanks = 1;
  o->provider = "cpu";
  if (argc < 2)
    Die("usage: sherpa_bench info|offline|stream|kws|tts|listen ...", 0);
  o->mode = argv[1];
  for (int i = 2; i < argc; ++i) {
    const char *a = argv[i];
#define STR(flag, field) \
  if (!strcmp(a, flag)) { o->field = Need(argc, argv, &i); continue; }
#define FLT(flag, field) \
  if (!strcmp(a, flag)) { o->field = (float)atof(Need(argc, argv, &i)); continue; }
#define INT(flag, field) \
  if (!strcmp(a, flag)) { o->field = atoi(Need(argc, argv, &i)); continue; }
    STR("--list", list) INT("--threads", threads) STR("--kind", kind)
    STR("--encoder", encoder) STR("--decoder", decoder) STR("--joiner", joiner)
    STR("--tokens", tokens) STR("--model-type", model_type)
    STR("--modeling-unit", modeling_unit) STR("--bpe-vocab", bpe_vocab)
    STR("--preprocessor", preprocessor)
    STR("--uncached-decoder", uncached_decoder)
    STR("--cached-decoder", cached_decoder)
    STR("--merged-decoder", merged_decoder)
    STR("--vad", vad) FLT("--vad-silence", vad_silence)
    FLT("--vad-threshold", vad_threshold)
    INT("--chunk-ms", chunk_ms) INT("--tail-pad-ms", tail_pad_ms)
    STR("--decoding", decoding)
    FLT("--hotwords-score", hotwords_score) STR("--keywords", keywords)
    FLT("--kws-threshold", kws_threshold) FLT("--kws-score", kws_score)
    INT("--kws-trailing-blanks", kws_trailing_blanks)
    STR("--model", model) STR("--voices", voices) STR("--lexicon", lexicon)
    STR("--vocoder", vocoder)
    INT("--tokens-from-buffer", tokens_from_buffer)
    STR("--listen-engine", listen_engine)
    if (!strcmp(a, "--provider-config")) {
      static char provider[4096];
      snprintf(provider, sizeof(provider), "cpu:%s", Need(argc, argv, &i));
      o->provider = provider;
      continue;
    }
#undef STR
#undef FLT
#undef INT
    Die("unknown option", a);
  }
  if (o->threads < 1 || o->threads > 64) Die("bad --threads", 0);
  if (o->chunk_ms < 10 || o->chunk_ms > 2000) Die("bad --chunk-ms", 0);
  if (o->tail_pad_ms < 0 || o->tail_pad_ms > 3000) Die("bad --tail-pad-ms", 0);
}

// ---------------------------------------------------------------- list files

typedef struct Item {
  char *fields[6];
  int n;
} Item;

typedef struct List {
  Item *items;
  int n;
} List;

static List ReadList(const char *path) {
  List l = {0, 0};
  int cap = 0;
  FILE *f = fopen(path, "rb");
  if (!f) Die("cannot open list", path);
  static char line[1 << 16];
  while (fgets(line, sizeof(line), f)) {
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = 0;
    if (!len || line[0] == '#') continue;
    if (l.n == cap) {
      cap = cap ? cap * 2 : 64;
      l.items = (Item *)realloc(l.items, sizeof(Item) * (size_t)cap);
      if (!l.items) Die("out of memory", 0);
    }
    Item *it = &l.items[l.n++];
    memset(it, 0, sizeof(*it));
    char *s = line;
    while (it->n < 6) {
      char *tab = strchr(s, '\t');
      if (tab) *tab = 0;
      size_t flen = strlen(s);
      it->fields[it->n] = (char *)malloc(flen + 1);
      if (!it->fields[it->n]) Die("out of memory", 0);
      memcpy(it->fields[it->n], s, flen + 1);
      it->n++;
      if (!tab) break;
      s = tab + 1;
    }
  }
  fclose(f);
  return l;
}

static char *ReadWholeFile(const char *path, int32_t *size) {
  FILE *f = fopen(path, "rb");
  if (!f) Die("cannot open", path);
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = (char *)malloc((size_t)n + 1);
  if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) Die("cannot read", path);
  buf[n] = 0;
  fclose(f);
  *size = (int32_t)n;
  return buf;
}

static void PrintLoad(const char *what, double load_ms) {
  printf("{\"event\":\"load\",\"what\":");
  PrintJsonString(what);
  printf(",\"load_ms\":%.2f,\"rss_after_load_kb\":%ld,\"version\":",
         load_ms, CurrentRssKb());
  PrintJsonString(SherpaOnnxGetVersionStr());
  printf(",\"onnxruntime\":");
  PrintJsonString(SherpaOnnxGetOnnxruntimeVersionStr());
  printf("}\n");
  fflush(stdout);
}

static void PrintEnd(int items, double total_ms) {
  printf("{\"event\":\"end\",\"items\":%d,\"total_ms\":%.2f,"
         "\"peak_rss_kb\":%ld}\n",
         items, total_ms, PeakRssKb());
  fflush(stdout);
}

// Silero VAD as the speech service would configure it: 0.5 threshold,
// --vad-silence of trailing silence closes a segment.
static const SherpaOnnxVoiceActivityDetector *CreateVad(const Options *o) {
  SherpaOnnxVadModelConfig vc;
  memset(&vc, 0, sizeof(vc));
  vc.silero_vad.model = o->vad;
  vc.silero_vad.threshold = o->vad_threshold;
  vc.silero_vad.min_silence_duration = o->vad_silence;
  vc.silero_vad.min_speech_duration = 0.1f;
  vc.silero_vad.max_speech_duration = 20.0f;
  vc.silero_vad.window_size = 512;
  vc.sample_rate = 16000;
  vc.num_threads = 1;
  vc.provider = "cpu";
  const SherpaOnnxVoiceActivityDetector *vad =
      SherpaOnnxCreateVoiceActivityDetector(&vc, 60.0f);
  if (!vad) Die("cannot create the VAD", o->vad);
  return vad;
}

// ---------------------------------------------------------------- offline

static int RunOffline(const Options *o) {
  SherpaOnnxOfflineRecognizerConfig c;
  memset(&c, 0, sizeof(c));
  c.feat_config.sample_rate = 16000;
  c.feat_config.feature_dim = 80;
  c.model_config.tokens = o->tokens;
  c.model_config.num_threads = o->threads;
  c.model_config.provider = o->provider;
  c.decoding_method = o->decoding;
  c.max_active_paths = 4;
  const char *kind = o->kind ? o->kind : "transducer";
  if (!strcmp(kind, "transducer") || !strcmp(kind, "nemo_transducer")) {
    c.model_config.transducer.encoder = o->encoder;
    c.model_config.transducer.decoder = o->decoder;
    c.model_config.transducer.joiner = o->joiner;
    if (!strcmp(kind, "nemo_transducer"))
      c.model_config.model_type = "nemo_transducer";
  } else if (!strcmp(kind, "moonshine")) {
    c.model_config.moonshine.preprocessor = o->preprocessor;
    c.model_config.moonshine.encoder = o->encoder;
    c.model_config.moonshine.uncached_decoder = o->uncached_decoder;
    c.model_config.moonshine.cached_decoder = o->cached_decoder;
  } else if (!strcmp(kind, "moonshine2")) {
    c.model_config.moonshine.encoder = o->encoder;
    c.model_config.moonshine.merged_decoder = o->merged_decoder;
  } else if (!strcmp(kind, "whisper")) {
    c.model_config.whisper.encoder = o->encoder;
    c.model_config.whisper.decoder = o->decoder;
    c.model_config.whisper.language = "en";
    c.model_config.whisper.task = "transcribe";
    c.model_config.whisper.tail_paddings = -1;
  } else {
    Die("unknown --kind", kind);
  }
  if (o->model_type) c.model_config.model_type = o->model_type;
  if (o->modeling_unit) c.model_config.modeling_unit = o->modeling_unit;
  if (o->bpe_vocab) c.model_config.bpe_vocab = o->bpe_vocab;

  List l = ReadList(o->list);
  double t0 = NowMs();
  const SherpaOnnxOfflineRecognizer *rec = SherpaOnnxCreateOfflineRecognizer(&c);
  double load_ms = NowMs() - t0;
  if (!rec) Die("cannot create the offline recognizer", kind);
  const SherpaOnnxVoiceActivityDetector *vad = NULL;
  if (o->vad) {
    double v0 = NowMs();
    vad = CreateVad(o);
    load_ms += NowMs() - v0;
  }
  PrintLoad(kind, load_ms);

  double start = NowMs();
  for (int i = 0; i < l.n; ++i) {
    if (l.items[i].n < 2) Die("bad list line", l.items[i].fields[0]);
    const SherpaOnnxWave *w = SherpaOnnxReadWave(l.items[i].fields[1]);
    if (!w) Die("cannot read wav", l.items[i].fields[1]);
    // As deployed, a per-utterance recognizer gets what the VAD found:
    // from the first speech segment's start to the last one's end, with
    // 0.25 s of the audio around it (the service keeps a short pre-roll).
    int32_t from = 0, to = w->num_samples;
    double vad_ms = 0;
    int segments = -1;
    if (vad) {
      double v = NowMs();
      SherpaOnnxVoiceActivityDetectorReset(vad);
      for (int32_t p = 0; p + 512 <= w->num_samples; p += 512)
        SherpaOnnxVoiceActivityDetectorAcceptWaveform(vad, w->samples + p, 512);
      SherpaOnnxVoiceActivityDetectorFlush(vad);
      int64_t lo = -1, hi = -1;
      segments = 0;
      while (!SherpaOnnxVoiceActivityDetectorEmpty(vad)) {
        const SherpaOnnxSpeechSegment *seg = SherpaOnnxVoiceActivityDetectorFront(vad);
        if (seg) {
          if (lo < 0 || seg->start < lo) lo = seg->start;
          if (seg->start + seg->n > hi) hi = seg->start + seg->n;
          segments++;
          SherpaOnnxDestroySpeechSegment(seg);
        }
        SherpaOnnxVoiceActivityDetectorPop(vad);
      }
      vad_ms = NowMs() - v;
      if (segments == 0) {
        from = to = 0;
      } else {
        from = (int32_t)(lo - 4000 > 0 ? lo - 4000 : 0);
        to = (int32_t)(hi + 4000 < w->num_samples ? hi + 4000 : w->num_samples);
      }
    }
    const char *text = "";
    const SherpaOnnxOfflineRecognizerResult *r = NULL;
    const SherpaOnnxOfflineStream *s = NULL;
    double a = NowMs();
    if (to > from) {
      s = SherpaOnnxCreateOfflineStream(rec);
      SherpaOnnxAcceptWaveformOffline(s, w->sample_rate, w->samples + from,
                                      to - from);
      SherpaOnnxDecodeOfflineStream(rec, s);
      r = SherpaOnnxGetOfflineStreamResult(s);
      if (r && r->text) text = r->text;
    }
    double decode_ms = NowMs() - a;
    printf("{\"event\":\"utt\",\"id\":");
    PrintJsonString(l.items[i].fields[0]);
    printf(",\"audio_s\":%.3f,\"decoded_s\":%.3f,\"decode_ms\":%.2f",
           (double)w->num_samples / w->sample_rate,
           (double)(to - from) / w->sample_rate, decode_ms);
    if (vad) printf(",\"vad_ms\":%.2f,\"vad_segments\":%d", vad_ms, segments);
    printf(",\"text\":");
    PrintJsonString(text);
    printf("}\n");
    fflush(stdout);
    if (r) SherpaOnnxDestroyOfflineRecognizerResult(r);
    if (s) SherpaOnnxDestroyOfflineStream(s);
    SherpaOnnxFreeWave(w);
  }
  PrintEnd(l.n, NowMs() - start);
  if (vad) SherpaOnnxDestroyVoiceActivityDetector(vad);
  SherpaOnnxDestroyOfflineRecognizer(rec);
  return 0;
}

// ---------------------------------------------------------------- streaming

static void FillOnlineModel(const Options *o, SherpaOnnxOnlineModelConfig *m,
                            char **tokens_buf) {
  m->transducer.encoder = o->encoder;
  m->transducer.decoder = o->decoder;
  m->transducer.joiner = o->joiner;
  m->num_threads = o->threads;
  m->provider = o->provider;
  if (o->model_type) m->model_type = o->model_type;
  if (o->modeling_unit) m->modeling_unit = o->modeling_unit;
  if (o->bpe_vocab) m->bpe_vocab = o->bpe_vocab;
  if (o->tokens_from_buffer) {
    // The service would receive tokens.txt as a read-only handle and pass
    // its bytes; the C API accepts them in memory (tokens_buf).
    int32_t n = 0;
    *tokens_buf = ReadWholeFile(o->tokens, &n);
    m->tokens_buf = *tokens_buf;
    m->tokens_buf_size = n;
  } else {
    m->tokens = o->tokens;
  }
}

static int RunStream(const Options *o) {
  SherpaOnnxOnlineRecognizerConfig c;
  memset(&c, 0, sizeof(c));
  char *tokens_buf = NULL;
  c.feat_config.sample_rate = 16000;
  c.feat_config.feature_dim = 80;
  FillOnlineModel(o, &c.model_config, &tokens_buf);
  c.decoding_method = o->decoding;
  c.max_active_paths = 4;
  c.hotwords_score = o->hotwords_score;
  // The recognizer's own endpointing, reported for comparison with the VAD.
  c.enable_endpoint = 1;
  c.rule1_min_trailing_silence = 2.4f;
  c.rule2_min_trailing_silence = 1.2f;
  c.rule3_min_utterance_length = 20.0f;

  List l = ReadList(o->list);
  double t0 = NowMs();
  const SherpaOnnxOnlineRecognizer *rec = SherpaOnnxCreateOnlineRecognizer(&c);
  double load_ms = NowMs() - t0;
  if (!rec) Die("cannot create the streaming recognizer", o->encoder);

  const SherpaOnnxVoiceActivityDetector *vad = NULL;
  if (o->vad) {
    double v0 = NowMs();
    vad = CreateVad(o);
    load_ms += NowMs() - v0;
  }
  PrintLoad("stream", load_ms);

  const int chunk = 16000 * o->chunk_ms / 1000;
  // Silence after the end of speech, so that the model's last chunk (and
  // its look-ahead) is complete: about one chunk of the model.
  const int tail = 16 * o->tail_pad_ms;
  float *zeros = (float *)calloc(tail > 0 ? tail : 1, sizeof(float));
  if (!zeros) Die("out of memory", 0);
  double start = NowMs();
  for (int i = 0; i < l.n; ++i) {
    Item *it = &l.items[i];
    if (it->n < 4) Die("bad stream list line", it->fields[0]);
    const SherpaOnnxWave *w = SherpaOnnxReadWave(it->fields[1]);
    if (!w || w->sample_rate != 16000) Die("need a 16 kHz wav", it->fields[1]);
    double speech_start = atof(it->fields[2]);
    double speech_end = atof(it->fields[3]);
    const char *hotwords = it->n >= 5 && it->fields[4][0] ? it->fields[4] : NULL;
    double cs = NowMs();
    const SherpaOnnxOnlineStream *s =
        hotwords ? SherpaOnnxCreateOnlineStreamWithHotwords(rec, hotwords)
                 : SherpaOnnxCreateOnlineStream(rec);
    double stream_create_ms = NowMs() - cs;
    if (vad) SherpaOnnxVoiceActivityDetectorReset(vad);

    double done = 0;  // simulated wall clock, ms of audio time
    double compute_total = 0;
    double first_partial_done = -1, first_partial_audio = -1;
    double vad_end_done = -1, vad_end_audio = -1;
    double endpoint_audio = -1;
    double final_done = -1;
    int vad_pos = 0;  // samples given to the VAD
    char *last_text = NULL;
    int32_t n = w->num_samples;
    for (int32_t pos = 0; pos < n; pos += chunk) {
      int32_t m = n - pos < chunk ? n - pos : chunk;
      double audio_ms = (double)(pos + m) * 1000.0 / 16000.0;
      double a = NowMs();
      SherpaOnnxOnlineStreamAcceptWaveform(s, 16000, w->samples + pos, m);
      while (SherpaOnnxIsOnlineStreamReady(rec, s))
        SherpaOnnxDecodeOnlineStream(rec, s);
      const SherpaOnnxOnlineRecognizerResult *r =
          SherpaOnnxGetOnlineStreamResult(rec, s);
      int vad_fired = 0;
      if (vad) {
        // Silero takes 512-sample windows.
        while (vad_pos + 512 <= pos + m) {
          SherpaOnnxVoiceActivityDetectorAcceptWaveform(vad, w->samples + vad_pos,
                                                        512);
          vad_pos += 512;
        }
        if (vad_end_audio < 0 && !SherpaOnnxVoiceActivityDetectorEmpty(vad))
          vad_fired = 1;
      }
      int endpoint = SherpaOnnxOnlineStreamIsEndpoint(rec, s);
      double c_ms = NowMs() - a;
      compute_total += c_ms;
      done = (audio_ms > done ? audio_ms : done) + c_ms;
      if (first_partial_done < 0 && r && r->text && r->text[0]) {
        first_partial_done = done;
        first_partial_audio = audio_ms;
      }
      if (endpoint && endpoint_audio < 0 && audio_ms / 1000.0 > speech_start)
        endpoint_audio = audio_ms;
      free(last_text);
      last_text = NULL;
      if (r && r->text) {
        size_t tl = strlen(r->text);
        last_text = (char *)malloc(tl + 1);
        if (last_text) memcpy(last_text, r->text, tl + 1);
      }
      SherpaOnnxDestroyOnlineRecognizerResult(r);
      if (vad_fired && audio_ms / 1000.0 > speech_start) {
        vad_end_audio = audio_ms;
        vad_end_done = done;
        // End of speech: flush the model's look-ahead with silence and
        // take the final result, as the service would.
        double f = NowMs();
        SherpaOnnxOnlineStreamAcceptWaveform(s, 16000, zeros, tail);
        SherpaOnnxOnlineStreamInputFinished(s);
        while (SherpaOnnxIsOnlineStreamReady(rec, s))
          SherpaOnnxDecodeOnlineStream(rec, s);
        const SherpaOnnxOnlineRecognizerResult *fr =
            SherpaOnnxGetOnlineStreamResult(rec, s);
        double f_ms = NowMs() - f;
        compute_total += f_ms;
        final_done = done + f_ms;
        free(last_text);
        last_text = NULL;
        if (fr && fr->text) {
          size_t tl = strlen(fr->text);
          last_text = (char *)malloc(tl + 1);
          if (last_text) memcpy(last_text, fr->text, tl + 1);
        }
        SherpaOnnxDestroyOnlineRecognizerResult(fr);
        break;
      }
    }
    if (final_done < 0) {
      // No VAD (or it never closed the segment): finish at the end of input.
      double f = NowMs();
      SherpaOnnxOnlineStreamAcceptWaveform(s, 16000, zeros, tail);
      SherpaOnnxOnlineStreamInputFinished(s);
      while (SherpaOnnxIsOnlineStreamReady(rec, s))
        SherpaOnnxDecodeOnlineStream(rec, s);
      const SherpaOnnxOnlineRecognizerResult *fr =
          SherpaOnnxGetOnlineStreamResult(rec, s);
      double f_ms = NowMs() - f;
      compute_total += f_ms;
      double end_audio = (double)n * 1000.0 / 16000.0;
      final_done = (end_audio > done ? end_audio : done) + f_ms;
      free(last_text);
      last_text = NULL;
      if (fr && fr->text) {
        size_t tl = strlen(fr->text);
        last_text = (char *)malloc(tl + 1);
        if (last_text) memcpy(last_text, fr->text, tl + 1);
      }
      SherpaOnnxDestroyOnlineRecognizerResult(fr);
    }
    printf("{\"event\":\"utt\",\"id\":");
    PrintJsonString(it->fields[0]);
    printf(",\"audio_s\":%.3f,\"compute_ms\":%.2f,\"stream_create_ms\":%.2f,"
           "\"biased\":%d",
           (double)n / 16000.0, compute_total, stream_create_ms,
           hotwords ? 1 : 0);
    if (first_partial_done >= 0)
      printf(",\"first_partial_ms\":%.1f,\"first_partial_audio_ms\":%.1f",
             first_partial_done - speech_start * 1000.0,
             first_partial_audio - speech_start * 1000.0);
    if (vad_end_done >= 0)
      printf(",\"vad_end_after_speech_ms\":%.1f",
             vad_end_audio - speech_end * 1000.0);
    printf(",\"final_after_speech_ms\":%.1f", final_done - speech_end * 1000.0);
    if (endpoint_audio >= 0)
      printf(",\"endpoint_after_speech_ms\":%.1f",
             endpoint_audio - speech_end * 1000.0);
    printf(",\"text\":");
    PrintJsonString(last_text ? last_text : "");
    printf("}\n");
    fflush(stdout);
    free(last_text);
    SherpaOnnxDestroyOnlineStream(s);
    SherpaOnnxFreeWave(w);
  }
  PrintEnd(l.n, NowMs() - start);
  free(zeros);
  free(tokens_buf);
  if (vad) SherpaOnnxDestroyVoiceActivityDetector(vad);
  SherpaOnnxDestroyOnlineRecognizer(rec);
  return 0;
}

// ---------------------------------------------------------------- keywords

static int RunKws(const Options *o) {
  SherpaOnnxKeywordSpotterConfig c;
  memset(&c, 0, sizeof(c));
  char *tokens_buf = NULL;
  c.feat_config.sample_rate = 16000;
  c.feat_config.feature_dim = 80;
  FillOnlineModel(o, &c.model_config, &tokens_buf);
  c.max_active_paths = 4;
  c.num_trailing_blanks = o->kws_trailing_blanks;
  c.keywords_score = o->kws_score;
  c.keywords_threshold = o->kws_threshold;
  if (!o->keywords) Die("kws needs --keywords", 0);
  c.keywords_file = o->keywords;

  List l = ReadList(o->list);
  double t0 = NowMs();
  const SherpaOnnxKeywordSpotter *kws = SherpaOnnxCreateKeywordSpotter(&c);
  double load_ms = NowMs() - t0;
  if (!kws) Die("cannot create the keyword spotter", o->keywords);
  PrintLoad("kws", load_ms);

  const int chunk = 16000 * o->chunk_ms / 1000;
  float *zeros = (float *)calloc(16000, sizeof(float));
  double start = NowMs();
  for (int i = 0; i < l.n; ++i) {
    const SherpaOnnxWave *w = SherpaOnnxReadWave(l.items[i].fields[1]);
    if (!w || w->sample_rate != 16000) Die("need a 16 kHz wav", l.items[i].fields[1]);
    const SherpaOnnxOnlineStream *s = SherpaOnnxCreateKeywordStream(kws);
    double compute = 0;
    printf("{\"event\":\"kws\",\"id\":");
    PrintJsonString(l.items[i].fields[0]);
    printf(",\"audio_s\":%.3f,\"hits\":[", (double)w->num_samples / 16000.0);
    int hits = 0;
    int32_t n = w->num_samples;
    for (int32_t pos = 0; pos <= n; pos += chunk) {
      double a = NowMs();
      if (pos < n) {
        int32_t m = n - pos < chunk ? n - pos : chunk;
        SherpaOnnxOnlineStreamAcceptWaveform(s, 16000, w->samples + pos, m);
      } else {
        SherpaOnnxOnlineStreamAcceptWaveform(s, 16000, zeros, 8000);
        SherpaOnnxOnlineStreamInputFinished(s);
      }
      while (SherpaOnnxIsKeywordStreamReady(kws, s)) {
        SherpaOnnxDecodeKeywordStream(kws, s);
        const SherpaOnnxKeywordResult *r = SherpaOnnxGetKeywordResult(kws, s);
        if (r && r->keyword && r->keyword[0]) {
          double at = (double)(pos < n ? pos : n) / 16000.0;
          printf("%s{\"keyword\":", hits ? "," : "");
          PrintJsonString(r->keyword);
          printf(",\"at_s\":%.2f,\"start_s\":%.2f}", at, r->start_time);
          hits++;
          SherpaOnnxResetKeywordStream(kws, s);
        }
        SherpaOnnxDestroyKeywordResult(r);
      }
      compute += NowMs() - a;
    }
    printf("],\"compute_ms\":%.2f}\n", compute);
    fflush(stdout);
    SherpaOnnxDestroyOnlineStream(s);
    SherpaOnnxFreeWave(w);
  }
  PrintEnd(l.n, NowMs() - start);
  free(zeros);
  free(tokens_buf);
  SherpaOnnxDestroyKeywordSpotter(kws);
  return 0;
}

// ---------------------------------------------------------------- TTS

typedef struct TtsProgress {
  double start;
  double first_ms;
} TtsProgress;

static int32_t OnAudio(const float *samples, int32_t n, float p, void *arg) {
  (void)samples;
  (void)p;
  TtsProgress *t = (TtsProgress *)arg;
  if (t->first_ms < 0 && n > 0) t->first_ms = NowMs() - t->start;
  return 1;
}

static int RunTts(const Options *o) {
  SherpaOnnxOfflineTtsConfig c;
  memset(&c, 0, sizeof(c));
  c.model.num_threads = o->threads;
  c.model.provider = o->provider;
  c.max_num_sentences = 1;
  const char *kind = o->kind ? o->kind : "";
  if (!strcmp(kind, "kokoro")) {
    c.model.kokoro.model = o->model;
    c.model.kokoro.voices = o->voices;
    c.model.kokoro.tokens = o->tokens;
    c.model.kokoro.lexicon = o->lexicon;
    c.model.kokoro.length_scale = 1.0f;
  } else if (!strcmp(kind, "vits")) {
    c.model.vits.model = o->model;
    c.model.vits.tokens = o->tokens;
    c.model.vits.lexicon = o->lexicon;
    c.model.vits.noise_scale = 0.667f;
    c.model.vits.noise_scale_w = 0.8f;
    c.model.vits.length_scale = 1.0f;
  } else if (!strcmp(kind, "matcha")) {
    c.model.matcha.acoustic_model = o->model;
    c.model.matcha.vocoder = o->vocoder;
    c.model.matcha.tokens = o->tokens;
    c.model.matcha.lexicon = o->lexicon;
    c.model.matcha.noise_scale = 0.667f;
    c.model.matcha.length_scale = 1.0f;
  } else {
    Die("tts needs --kind kokoro|vits|matcha", 0);
  }
  List l = ReadList(o->list);
  double t0 = NowMs();
  const SherpaOnnxOfflineTts *tts = SherpaOnnxCreateOfflineTts(&c);
  double load_ms = NowMs() - t0;
  if (!tts) Die("cannot create the TTS engine", o->model);
  PrintLoad(kind, load_ms);
  int32_t sr = SherpaOnnxOfflineTtsSampleRate(tts);
  double start = NowMs();
  for (int i = 0; i < l.n; ++i) {
    Item *it = &l.items[i];
    if (it->n < 4) Die("bad tts list line", it->fields[0]);
    SherpaOnnxGenerationConfig g;
    memset(&g, 0, sizeof(g));
    g.silence_scale = 0.2f;
    g.sid = atoi(it->fields[1]);
    g.speed = (float)atof(it->fields[2]);
    TtsProgress prog = {0, -1};
    prog.start = NowMs();
    const SherpaOnnxGeneratedAudio *a = SherpaOnnxOfflineTtsGenerateWithConfig(
        tts, it->fields[3], &g, OnAudio, &prog);
    double gen_ms = NowMs() - prog.start;
    if (!a) Die("generation failed", it->fields[0]);
    double audio_s = (double)a->n / (a->sample_rate ? a->sample_rate : sr);
    if (it->n >= 5 && it->fields[4][0]) {
      if (!SherpaOnnxWriteWave(a->samples, a->n, a->sample_rate, it->fields[4]))
        Die("cannot write", it->fields[4]);
    }
    printf("{\"event\":\"tts\",\"id\":");
    PrintJsonString(it->fields[0]);
    printf(",\"sample_rate\":%d,\"audio_s\":%.3f,\"gen_ms\":%.2f,"
           "\"first_audio_ms\":%.2f,\"chars\":%d}\n",
           a->sample_rate, audio_s, gen_ms, prog.first_ms,
           (int)strlen(it->fields[3]));
    fflush(stdout);
    SherpaOnnxDestroyOfflineTtsGeneratedAudio(a);
  }
  PrintEnd(l.n, NowMs() - start);
  SherpaOnnxDestroyOfflineTts(tts);
  return 0;
}

// ---------------------------------------------------------------- listen

// Always-on listening in real time: the audio is fed at the speed a
// microphone delivers it (the process sleeps in between), and the CPU time
// the process uses is compared with the wall time. "kws": the wake-phrase
// spotter (and the VAD, if given) waiting for the phrase; "stream": a
// streaming recognizer (and the VAD) transcribing everything, as in the
// follow-up window or always-listening mode.
static int RunListen(const Options *o) {
  const char *engine = o->listen_engine ? o->listen_engine : "kws";
  char *tokens_buf = NULL;
  const SherpaOnnxKeywordSpotter *kws = NULL;
  const SherpaOnnxOnlineRecognizer *rec = NULL;
  double t0 = NowMs();
  if (!strcmp(engine, "kws")) {
    SherpaOnnxKeywordSpotterConfig c;
    memset(&c, 0, sizeof(c));
    c.feat_config.sample_rate = 16000;
    c.feat_config.feature_dim = 80;
    FillOnlineModel(o, &c.model_config, &tokens_buf);
    c.max_active_paths = 4;
    c.num_trailing_blanks = o->kws_trailing_blanks;
    c.keywords_score = o->kws_score;
    c.keywords_threshold = o->kws_threshold;
    if (!o->keywords) Die("listen kws needs --keywords", 0);
    c.keywords_file = o->keywords;
    kws = SherpaOnnxCreateKeywordSpotter(&c);
    if (!kws) Die("cannot create the keyword spotter", o->keywords);
  } else if (!strcmp(engine, "stream")) {
    SherpaOnnxOnlineRecognizerConfig c;
    memset(&c, 0, sizeof(c));
    c.feat_config.sample_rate = 16000;
    c.feat_config.feature_dim = 80;
    FillOnlineModel(o, &c.model_config, &tokens_buf);
    c.decoding_method = o->decoding;
    c.max_active_paths = 4;
    c.enable_endpoint = 1;
    c.rule1_min_trailing_silence = 2.4f;
    c.rule2_min_trailing_silence = 1.2f;
    c.rule3_min_utterance_length = 20.0f;
    rec = SherpaOnnxCreateOnlineRecognizer(&c);
    if (!rec) Die("cannot create the streaming recognizer", o->encoder);
  } else {
    Die("unknown --listen-engine", engine);
  }
  const SherpaOnnxVoiceActivityDetector *vad = o->vad ? CreateVad(o) : NULL;
  PrintLoad(engine, NowMs() - t0);

  List l = ReadList(o->list);
  const int chunk = 16000 * o->chunk_ms / 1000;
  for (int i = 0; i < l.n; ++i) {
    const SherpaOnnxWave *w = SherpaOnnxReadWave(l.items[i].fields[1]);
    if (!w || w->sample_rate != 16000) Die("need a 16 kHz wav", l.items[i].fields[1]);
    const SherpaOnnxOnlineStream *s =
        kws ? SherpaOnnxCreateKeywordStream(kws) : SherpaOnnxCreateOnlineStream(rec);
    int events = 0;
    int vad_pos = 0;
    double late_ms = 0;
    double cpu0 = CpuSeconds();
    double start = NowMs();
    for (int32_t pos = 0; pos < w->num_samples; pos += chunk) {
      int32_t m = w->num_samples - pos < chunk ? w->num_samples - pos : chunk;
      double due = start + (double)(pos + m) * 1000.0 / 16000.0;
      SleepUntilMs(due);
      double behind = NowMs() - due;
      if (behind > late_ms) late_ms = behind;
      SherpaOnnxOnlineStreamAcceptWaveform(s, 16000, w->samples + pos, m);
      if (vad) {
        while (vad_pos + 512 <= pos + m) {
          SherpaOnnxVoiceActivityDetectorAcceptWaveform(vad, w->samples + vad_pos, 512);
          vad_pos += 512;
        }
        while (!SherpaOnnxVoiceActivityDetectorEmpty(vad))
          SherpaOnnxVoiceActivityDetectorPop(vad);
      }
      if (kws) {
        while (SherpaOnnxIsKeywordStreamReady(kws, s)) {
          SherpaOnnxDecodeKeywordStream(kws, s);
          const SherpaOnnxKeywordResult *r = SherpaOnnxGetKeywordResult(kws, s);
          if (r && r->keyword && r->keyword[0]) {
            events++;
            SherpaOnnxResetKeywordStream(kws, s);
          }
          SherpaOnnxDestroyKeywordResult(r);
        }
      } else {
        while (SherpaOnnxIsOnlineStreamReady(rec, s))
          SherpaOnnxDecodeOnlineStream(rec, s);
        if (SherpaOnnxOnlineStreamIsEndpoint(rec, s)) {
          events++;
          SherpaOnnxOnlineStreamReset(rec, s);
        }
      }
    }
    double wall = (NowMs() - start) / 1000.0;
    double cpu = CpuSeconds() - cpu0;
    printf("{\"event\":\"listen\",\"id\":");
    PrintJsonString(l.items[i].fields[0]);
    printf(",\"engine\":\"%s\",\"audio_s\":%.2f,\"wall_s\":%.2f,\"cpu_s\":%.3f,"
           "\"cpu_pct_of_one_core\":%.2f,\"max_late_ms\":%.1f,\"events\":%d}\n",
           engine, (double)w->num_samples / 16000.0, wall, cpu, 100.0 * cpu / wall,
           late_ms, events);
    fflush(stdout);
    SherpaOnnxDestroyOnlineStream(s);
    SherpaOnnxFreeWave(w);
  }
  PrintEnd(l.n, 0);
  free(tokens_buf);
  if (vad) SherpaOnnxDestroyVoiceActivityDetector(vad);
  if (kws) SherpaOnnxDestroyKeywordSpotter(kws);
  if (rec) SherpaOnnxDestroyOnlineRecognizer(rec);
  return 0;
}

int main(int argc, char **argv) {
  Options o;
  ParseOptions(argc, argv, &o);
  if (!strcmp(o.mode, "info")) {
    printf("{\"event\":\"info\",\"version\":");
    PrintJsonString(SherpaOnnxGetVersionStr());
    printf(",\"git\":");
    PrintJsonString(SherpaOnnxGetGitSha1());
    printf(",\"onnxruntime\":");
    PrintJsonString(SherpaOnnxGetOnnxruntimeVersionStr());
    printf(",\"rss_kb\":%ld}\n", CurrentRssKb());
    return 0;
  }
  if (!o.list) Die("missing --list", 0);
  if (!strcmp(o.mode, "offline")) return RunOffline(&o);
  if (!strcmp(o.mode, "stream")) return RunStream(&o);
  if (!strcmp(o.mode, "kws")) return RunKws(&o);
  if (!strcmp(o.mode, "tts")) return RunTts(&o);
  if (!strcmp(o.mode, "listen")) return RunListen(&o);
  Die("unknown mode", o.mode);
  return 2;
}
