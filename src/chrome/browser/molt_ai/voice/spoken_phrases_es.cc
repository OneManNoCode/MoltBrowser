// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Spanish: the words voice mode knows (spoken_phrases.h). Folded as
// spoken_text.h folds words: lower case, accents dropped, "ñ" as "n"
// ("pestaña" is "pestana", "último" is "ultimo").
//
// Numbers are built from units, teens, tens, whole hundreds and "mil":
// "veintiuno" and the other twenties are one word each (read as teens: no
// unit follows them), "treinta y uno" joins tens to a unit with "y",
// "ciento uno" and "doscientos veinte" need no joiner. Ordinals come in
// both genders ("el primero", "la primera", "el primer video") and up to
// "milésimo"; "vigésimo primero" is read as one ordinal.

#include <algorithm>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/voice/spoken_choice.h"
#include "chrome/browser/molt_ai/voice/spoken_phrases.h"

namespace molt_ai {
namespace voice {

namespace {

using K = NumberWordKind;

constexpr NumberWord kNumbers[] = {
    {"cero", K::kUnit, 0},
    {"uno", K::kUnit, 1},
    {"dos", K::kUnit, 2},
    {"tres", K::kUnit, 3},
    {"cuatro", K::kUnit, 4},
    {"cinco", K::kUnit, 5},
    {"seis", K::kUnit, 6},
    {"siete", K::kUnit, 7},
    {"ocho", K::kUnit, 8},
    {"nueve", K::kUnit, 9},
    {"diez", K::kTeen, 10},
    {"once", K::kTeen, 11},
    {"doce", K::kTeen, 12},
    {"trece", K::kTeen, 13},
    {"catorce", K::kTeen, 14},
    {"quince", K::kTeen, 15},
    {"dieciseis", K::kTeen, 16},
    {"diecisiete", K::kTeen, 17},
    {"dieciocho", K::kTeen, 18},
    {"diecinueve", K::kTeen, 19},
    {"veinte", K::kTens, 20},
    // The twenties are one word: no unit follows them.
    {"veintiuno", K::kTeen, 21},
    {"veintiun", K::kTeen, 21},
    {"veintiuna", K::kTeen, 21},
    {"veintidos", K::kTeen, 22},
    {"veintitres", K::kTeen, 23},
    {"veinticuatro", K::kTeen, 24},
    {"veinticinco", K::kTeen, 25},
    {"veintiseis", K::kTeen, 26},
    {"veintisiete", K::kTeen, 27},
    {"veintiocho", K::kTeen, 28},
    {"veintinueve", K::kTeen, 29},
    {"treinta", K::kTens, 30},
    {"cuarenta", K::kTens, 40},
    {"cincuenta", K::kTens, 50},
    {"sesenta", K::kTens, 60},
    {"setenta", K::kTens, 70},
    {"ochenta", K::kTens, 80},
    {"noventa", K::kTens, 90},
    {"cien", K::kHundreds, 100},
    {"ciento", K::kHundreds, 100},
    {"doscientos", K::kHundreds, 200},
    {"doscientas", K::kHundreds, 200},
    {"trescientos", K::kHundreds, 300},
    {"trescientas", K::kHundreds, 300},
    {"cuatrocientos", K::kHundreds, 400},
    {"cuatrocientas", K::kHundreds, 400},
    {"quinientos", K::kHundreds, 500},
    {"quinientas", K::kHundreds, 500},
    {"seiscientos", K::kHundreds, 600},
    {"seiscientas", K::kHundreds, 600},
    {"setecientos", K::kHundreds, 700},
    {"setecientas", K::kHundreds, 700},
    {"ochocientos", K::kHundreds, 800},
    {"ochocientas", K::kHundreds, 800},
    {"novecientos", K::kHundreds, 900},
    {"novecientas", K::kHundreds, 900},
    {"mil", K::kThousand, 1000},
    // Ordinals, both genders, and the short forms before a noun ("el
    // primer video", "el tercer resultado").
    {"primero", K::kUnit, 1, true},
    {"primera", K::kUnit, 1, true},
    {"primer", K::kUnit, 1, true},
    {"segundo", K::kUnit, 2, true},
    {"segunda", K::kUnit, 2, true},
    {"tercero", K::kUnit, 3, true},
    {"tercera", K::kUnit, 3, true},
    {"tercer", K::kUnit, 3, true},
    {"cuarto", K::kUnit, 4, true},
    {"cuarta", K::kUnit, 4, true},
    {"quinto", K::kUnit, 5, true},
    {"quinta", K::kUnit, 5, true},
    {"sexto", K::kUnit, 6, true},
    {"sexta", K::kUnit, 6, true},
    {"septimo", K::kUnit, 7, true},
    {"septima", K::kUnit, 7, true},
    {"setimo", K::kUnit, 7, true},
    {"setima", K::kUnit, 7, true},
    {"octavo", K::kUnit, 8, true},
    {"octava", K::kUnit, 8, true},
    {"noveno", K::kUnit, 9, true},
    {"novena", K::kUnit, 9, true},
    {"decimo", K::kTeen, 10, true},
    {"decima", K::kTeen, 10, true},
    {"undecimo", K::kTeen, 11, true},
    {"undecima", K::kTeen, 11, true},
    {"decimoprimero", K::kTeen, 11, true},
    {"decimoprimera", K::kTeen, 11, true},
    {"decimoprimer", K::kTeen, 11, true},
    {"onceavo", K::kTeen, 11, true},
    {"onceava", K::kTeen, 11, true},
    {"duodecimo", K::kTeen, 12, true},
    {"duodecima", K::kTeen, 12, true},
    {"decimosegundo", K::kTeen, 12, true},
    {"decimosegunda", K::kTeen, 12, true},
    {"doceavo", K::kTeen, 12, true},
    {"doceava", K::kTeen, 12, true},
    {"decimotercero", K::kTeen, 13, true},
    {"decimotercera", K::kTeen, 13, true},
    {"decimotercer", K::kTeen, 13, true},
    {"decimocuarto", K::kTeen, 14, true},
    {"decimocuarta", K::kTeen, 14, true},
    {"decimoquinto", K::kTeen, 15, true},
    {"decimoquinta", K::kTeen, 15, true},
    {"decimosexto", K::kTeen, 16, true},
    {"decimosexta", K::kTeen, 16, true},
    {"decimoseptimo", K::kTeen, 17, true},
    {"decimoseptima", K::kTeen, 17, true},
    {"decimoctavo", K::kTeen, 18, true},
    {"decimoctava", K::kTeen, 18, true},
    {"decimonoveno", K::kTeen, 19, true},
    {"decimonovena", K::kTeen, 19, true},
    {"vigesimo", K::kTens, 20, true},
    {"vigesima", K::kTens, 20, true},
    {"trigesimo", K::kTens, 30, true},
    {"trigesima", K::kTens, 30, true},
    {"cuadragesimo", K::kTens, 40, true},
    {"cuadragesima", K::kTens, 40, true},
    {"quincuagesimo", K::kTens, 50, true},
    {"quincuagesima", K::kTens, 50, true},
    {"sexagesimo", K::kTens, 60, true},
    {"sexagesima", K::kTens, 60, true},
    {"septuagesimo", K::kTens, 70, true},
    {"septuagesima", K::kTens, 70, true},
    {"octogesimo", K::kTens, 80, true},
    {"octogesima", K::kTens, 80, true},
    {"nonagesimo", K::kTens, 90, true},
    {"nonagesima", K::kTens, 90, true},
    {"centesimo", K::kHundreds, 100, true},
    {"centesima", K::kHundreds, 100, true},
    {"milesimo", K::kThousand, 1000, true},
    {"milesima", K::kThousand, 1000, true},
};

constexpr TimeUnitWord kTimeUnits[] = {
    {"segundo", 1}, {"segundos", 1}, {"seg", 1},      {"segs", 1},
    {"s", 1},       {"minuto", 60},  {"minutos", 60}, {"min", 60},
    {"mins", 60},   {"hora", 3600},  {"horas", 3600}, {"h", 3600},
    {"hr", 3600},   {"hrs", 3600},
};

constexpr ChoiceRoleMask kListish = 0;  // names an item, not its role

constexpr RoleWord kRoleWords[] = {
    {"resultado", kListish},
    {"resultados", kListish},
    {"elemento", kListish},
    {"elementos", kListish},
    {"entrada", kListish},
    {"opcion", kListish},
    {"opciones", kListish},
    {"cosa", kListish},
    {"item", kListish},
    {"video", RoleBit(ChoiceRole::kVideo)},
    {"videos", RoleBit(ChoiceRole::kVideo)},
    {"clip", RoleBit(ChoiceRole::kVideo)},
    {"cancion", RoleBit(ChoiceRole::kAudio)},
    {"tema", RoleBit(ChoiceRole::kAudio)},
    {"pista", RoleBit(ChoiceRole::kAudio)},
    {"episodio", RoleBit(ChoiceRole::kAudio) | RoleBit(ChoiceRole::kVideo)},
    {"podcast", RoleBit(ChoiceRole::kAudio)},
    {"enlace", RoleBit(ChoiceRole::kLink)},
    {"enlaces", RoleBit(ChoiceRole::kLink)},
    {"vinculo", RoleBit(ChoiceRole::kLink)},
    {"link", RoleBit(ChoiceRole::kLink)},
    {"boton", RoleBit(ChoiceRole::kButton)},
    {"campo", RoleBit(ChoiceRole::kTextField)},
    {"cuadro",
     RoleBit(ChoiceRole::kTextField) | RoleBit(ChoiceRole::kCheckBox)},
    {"casilla", RoleBit(ChoiceRole::kCheckBox)},
    {"desplegable", RoleBit(ChoiceRole::kListBox)},
    {"menu", RoleBit(ChoiceRole::kListBox)},
    {"pestana", RoleBit(ChoiceRole::kTab)},
    {"imagen", RoleBit(ChoiceRole::kImage)},
    {"foto", RoleBit(ChoiceRole::kImage)},
    {"fotografia", RoleBit(ChoiceRole::kImage)},
    {"encabezado", RoleBit(ChoiceRole::kHeading)},
    {"articulo", RoleBit(ChoiceRole::kArticle)},
    {"noticia", RoleBit(ChoiceRole::kArticle)},
    {"publicacion", RoleBit(ChoiceRole::kArticle)},
    {"producto", RoleBit(ChoiceRole::kProduct)},
};

constexpr std::string_view kChoiceStopwords[] = {
    "a",       "al",      "el",       "la",       "los",     "las",  "lo",
    "un",      "una",     "unos",     "unas",     "que",     "se",   "llama",
    "llamado", "llamada", "titulado", "titulada", "dice",    "pone", "es",
    "este",    "esta",    "ese",      "esa",      "eso",     "esto", "estos",
    "estas",   "esos",    "esas",     "aquel",    "aquella", "y",    "o",
    "ahi",     "alli",    "aqui",     "cual",     "donde",   "uno",  "unas",
    "otro",    "otra",    "sobre",
};

// Kept when an item has them ("Iniciar sesión" / "Cerrar sesión" is told
// apart by its verb; "en vivo"), and weightless when none does.
constexpr std::string_view kChoiceParticles[] = {
    "con", "en", "entre", "hacia", "hasta", "para", "sin",
};

}  // namespace

LanguagePhrases MakeSpanishPhrases() {
  LanguagePhrases p;
  p.language = Language::kSpanish;

  p.numbers.assign(std::begin(kNumbers), std::end(kNumbers));
  std::sort(
      p.numbers.begin(), p.numbers.end(),
      [](const NumberWord& a, const NumberWord& b) { return a.word < b.word; });
  // Between the parts of a length of time ("un minuto y treinta
  // segundos"); between tens and units it is |tens_unit_and|.
  p.number_and = {"y"};
  p.number_article = {"un", "una"};
  // "4º", "4ª" (folded "4o", "4a"), "1er", "2do", "3ro", "4to", "7mo".
  p.ordinal_suffixes = {"o",  "a",  "er", "era", "ero", "ro", "ra",
                        "do", "da", "to", "ta",  "vo",  "va", "no",
                        "na", "mo", "ma", "avo", "ava"};
  // "#4" and "No. 4" become "number" in spoken_text.h, in any language.
  p.number_prefixes = {"numero", "num", "nro", "number"};

  p.last_words = {"ultimo", "ultima"};
  p.end_phrases = {
      "desde el final",          "por el final", "empezando por el final",
      "contando desde el final", "desde abajo",  "contando desde abajo",
      "empezando por abajo"};
  p.second_to_last_phrases = {"penultimo", "penultima"};
  p.third_to_last_phrases = {"antepenultimo", "antepenultima"};
  p.position_intensifiers = {"mismisimo", "mismisima"};

  p.time_units.assign(std::begin(kTimeUnits), std::end(kTimeUnits));
  p.half_words = {"medio", "media"};
  p.and_half_phrases = {"y medio", "y media"};
  p.half_before_unit = true;

  p.articles = {"el", "la", "los", "las", "lo", "un", "una"};
  p.current_phrases = {"este",
                       "esta",
                       "ese",
                       "esa",
                       "eso",
                       "esto",
                       "el mismo",
                       "la misma",
                       "el actual",
                       "la actual",
                       "el seleccionado",
                       "la seleccionada",
                       "el que elegi",
                       "la que elegi",
                       "este de aqui",
                       "esta de aqui",
                       "este elemento",
                       "ese elemento"};
  p.next_phrases = {"el siguiente",         "la siguiente", "el proximo",
                    "la proxima",           "el de abajo",  "la de abajo",
                    "el de debajo",         "la de debajo", "el de despues",
                    "la de despues",        "el que sigue", "la que sigue",
                    "el siguiente elemento"};
  p.previous_phrases = {"el anterior",         "la anterior", "el de arriba",
                        "la de arriba",        "el de antes", "la de antes",
                        "el previo",           "la previa",   "el que va antes",
                        "el elemento anterior"};
  // "el de Queen", "el video en vivo de Queen": "de" names the author most
  // often. A "de" that names nobody ("el de 1985", "la de la película")
  // gives its words back to the description (spoken_choice.h).
  p.by_cues = {"de",
               "del",
               "por",
               "subido por",
               "publicado por",
               "escrito por",
               "cantado por",
               "interpretado por",
               "hecho por",
               "vendido por",
               "del canal",
               "en el canal"};
  p.from_cues = {"desde", "publicado en", "del ano", "lanzado en"};
  p.choice_stopwords.assign(std::begin(kChoiceStopwords),
                            std::end(kChoiceStopwords));
  std::sort(p.choice_stopwords.begin(), p.choice_stopwords.end());
  p.choice_stopwords.erase(
      std::unique(p.choice_stopwords.begin(), p.choice_stopwords.end()),
      p.choice_stopwords.end());
  p.choice_particles.assign(std::begin(kChoiceParticles),
                            std::end(kChoiceParticles));
  std::sort(p.choice_particles.begin(), p.choice_particles.end());
  p.role_words.assign(std::begin(kRoleWords), std::end(kRoleWords));
  std::sort(
      p.role_words.begin(), p.role_words.end(),
      [](const RoleWord& a, const RoleWord& b) { return a.word < b.word; });

  p.fillers = {"eh",  "em", "ehm", "emm", "mm", "mmm",
               "hmm", "ah", "uh",  "um",  "uhm"};
  p.leading_phrases = {"por favor",
                       "puedes",
                       "podrias",
                       "puede",
                       "podria",
                       "me puedes",
                       "me podrias",
                       "me puede",
                       "me podria",
                       "puedes por favor",
                       "podrias por favor",
                       "quiero",
                       "quisiera",
                       "me gustaria",
                       "necesito",
                       "vamos a",
                       "solo",
                       "ahora",
                       "y",
                       "luego",
                       "despues",
                       "entonces",
                       "pues",
                       "y luego",
                       "y despues"};
  p.trailing_phrases = {"por favor", "porfa", "gracias", "muchas gracias"};
  p.trailing_if_command = {"ahora", "ahora mismo", "ya"};
  p.leading_if_more = {"ok",    "okay",  "vale", "bueno", "bien",
                       "a ver", "venga", "oye",  "hola",  "oiga"};
  p.wake_greetings = {"oye", "oie", "hola", "ola", "hey",
                      "ey",  "ok",  "okay", "eh"};
  p.default_wake_phrase = "oye molt";

  p.volume_words = {{"la mitad", 50}, {"mitad", 50},     {"medio", 50},
                    {"maximo", 100},  {"tope", 100},     {"a tope", 100},
                    {"minimo", 0},    {"lo maximo", 100}};
  // "%" becomes "percent" in spoken_text.h.
  p.percent_words = {"por ciento", "porciento", "percent"};
  p.out_of_ten = {"de diez", "sobre diez", "de 10", "sobre 10"};
  p.page_words = {"pagina", "paginas", "pantalla", "pantallas"};
  p.scroll_half = {"media pagina", "media pantalla", "un poco",    "un poquito",
                   "un poco mas",  "un poquito mas", "ligeramente"};
  p.scroll_page = {"una pagina", "una pantalla", "pagina", "pantalla"};

  p.address_words = {
      {"punto", '.'}, {"barra", '/'}, {"guion", '-'}, {"subrayado", '_'}};
  p.site_suffixes = {"sitio web", "pagina web", "web",   "sitio",
                     "app",       "aplicacion", "portal"};
  p.site_prefixes = {"el",
                     "la",
                     "la web de",
                     "la pagina de",
                     "la pagina web de",
                     "el sitio de",
                     "el sitio web de",
                     "la app de",
                     "la aplicacion de",
                     "el portal de"};

  p.tens_unit_and = {"y"};
  p.compound_ordinals = true;
  p.years_in_two_parts = false;
  p.bare_number_after_article = true;
  // "Molt" isn't a Spanish word: a recognizer writes "mol", "molde",
  // "molts". Within a consonant or two vowels of it is the name.
  p.wake_sound_margin = 2;
  return p;
}

}  // namespace voice
}  // namespace molt_ai
