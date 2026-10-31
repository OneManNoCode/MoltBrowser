// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/confirmation_words_es.h"

#include <algorithm>
#include <array>
#include <string>

#include "chrome/browser/molt_ai/voice/phonetic_key.h"
#include "chrome/browser/molt_ai/voice/phonetic_key_es.h"

namespace molt_ai {
namespace voice {

namespace {

// Picked from 230 candidates by the rule in the header (greedily, common
// and short words first, against the answers, the leading words and the
// numbers and ordinals up to twenty); of the 50 that passed, words whose
// plural changes their last sound ("avestruz", "altavoz") and the longest
// or rarest ("hipopótamo", "albaricoque", "luciérnaga") were left out.
constexpr auto kWords = std::to_array<std::string_view>({
    "manzana",   "tomate",   "sombrero",  "castillo",    "molino",
    "jirafa",    "caballo",  "hormiga",   "caracol",     "platano",
    "chocolate", "martillo", "cactus",    "ancla",       "mochila",
    "tranvia",   "camion",   "tractor",   "submarino",   "delfin",
    "cangrejo",  "langosta", "mapache",   "arcoiris",    "relampago",
    "linterna",  "saxofon",  "mosquito",  "alfombra",    "ascensor",
    "bizcocho",  "yogur",    "mermelada", "mantequilla", "almendra",
    "melocoton", "halcon",   "tornillo",  "pulsera",     "salchicha",
});

// The answers of the Spanish prompts (voice_prompts.h's, in Spanish), the
// words said around them and the loop's yes and no, the wake phrase's
// words, and the numbers a list is chosen from. "yes", "ok", "stop":
// Spanish speakers say them too.
constexpr auto kAnswerWords = std::to_array<std::string_view>({
    "si",        "no",         "vale",    "ok",       "okay",
    "para",      "alto",       "cancela", "cancelar", "bloquea",
    "bloquear",  "bloquearlo", "permite", "permitir", "permitirlo",
    "deniega",   "denegar",    "sal",     "salir",    "quedate",
    "quedarme",  "quedar",     "guarda",  "guardar",  "nunca",
    "descarta",  "descartar",  "borra",   "borrar",   "elimina",
    "eliminar",  "repite",     "ayuda",   "molt",     "oye",
    "hola",      "hey",        "uno",     "una",      "un",
    "dos",       "tres",       "cuatro",  "cinco",    "seis",
    "siete",     "ocho",       "nueve",   "diez",     "once",
    "doce",      "siguiente",  "atras",   "responde", "respuesta",
    "responder", "envia",      "enviar",  "claro",    "seguro",
    "sip",       "pausa",      "abre",    "cierra",   "cerrar",
    "recarga",   "recargar",   "favor",   "por",      "gracias",
    "otra",      "vez",        "que",     "perdon",   "espera",
    "ahora",     "luego",      "tarde",   "ignora",   "ignorar",
    "confirma",  "confirmar",  "bien",    "venga",    "dale",
    "nop",       "escribe",    "pagina",  "aqui",     "contrasena",
    "es",        "mi",         "eso",     "esto",     "acepta",
    "aceptar",   "deja",       "dejar",   "di",       "yes",
    "stop",
});

constexpr auto kLeading = std::to_array<std::string_view>(
    {"si", "vale", "ok", "okay", "di", "es", "el", "la", "un", "una"});
constexpr auto kTrailing =
    std::to_array<std::string_view>({"por favor", "porfa", "gracias"});

bool KeysConfusable(const std::string& key_a,
                    const std::string& key_b,
                    int margin) {
  if (key_a == key_b) {
    return true;
  }
  const int bound =
      PhoneticBound(std::max(key_a.size(), key_b.size())) + margin;
  return PhoneticDistance(key_a, key_b, bound) <= bound;
}

}  // namespace

std::span<const std::string_view> SpanishConfirmationWords() {
  return kWords;
}

std::span<const std::string_view> SpanishAnswerWords() {
  return kAnswerWords;
}

std::span<const std::string_view> SpanishConfirmationLeading() {
  return kLeading;
}

std::span<const std::string_view> SpanishConfirmationTrailing() {
  return kTrailing;
}

bool SpanishWordsConfusable(std::string_view a,
                            std::string_view b,
                            int margin) {
  const std::string english_a = PhoneticKey(a);
  const std::string english_b = PhoneticKey(b);
  const std::string spanish_a = SpanishPhoneticKey(a);
  const std::string spanish_b = SpanishPhoneticKey(b);
  if (english_a.empty() || english_b.empty() || spanish_a.empty() ||
      spanish_b.empty()) {
    return a == b;
  }
  return KeysConfusable(english_a, english_b, margin) ||
         KeysConfusable(spanish_a, spanish_b, margin);
}

}  // namespace voice
}  // namespace molt_ai
