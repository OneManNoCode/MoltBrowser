// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Spanish: what the browser says (spoken_answers.h), keyed by the voice
// loop's English sentence. Familiar "tú", as the commands are said; the
// commands quoted in the answers are the Spanish grammar's own
// (voice_command_phrases_es.cc), so what the browser suggests works.
// tools/voice/check_spoken_answers.py checks that every sentence the loop
// says has one here, with the same placeholders.

#include <iterator>
#include <vector>

#include "chrome/browser/molt_ai/voice/spoken_answers.h"

namespace molt_ai {
namespace voice {

namespace {

constexpr SpokenAnswer kAnswers[] = {
    // --- Voice mode and the conversation ----------------------------------
    {"Voice mode is off in MoltNet windows",
     "El modo de voz está desactivado en las ventanas de MoltNet"},
    {"Voice mode is off in MoltNet windows.",
     "El modo de voz está desactivado en las ventanas de MoltNet."},
    {"That was too long for me. Say it in fewer words.",
     "Eso es demasiado largo para mí. Dilo con menos palabras."},
    {"I couldn't read that. Say it again.",
     "No he podido leer eso. Dilo otra vez."},
    {"There's nothing to confirm.", "No hay nada que confirmar."},
    {"Going ahead: {0}", "Adelante: {0}"},
    {"Okay, I won't.", "Vale, no lo haré."},
    {"Okay.", "Vale."},
    {"Cancelled", "Cancelado"},
    {"Cancelled.", "Cancelado."},
    {"Stopped", "Detenido"},
    {"Undo comes later. Say go back to return to the page before.",
     "Deshacer llegará más adelante. Di atrás para volver a la página "
     "anterior."},
    {"I haven't said anything yet.", "Todavía no he dicho nada."},
    {"I'm already listening.", "Ya estoy escuchando."},
    {"Turning on voice mode", "Activando el modo de voz"},
    {"I'm not listening now.", "Ahora no estoy escuchando."},
    {"Voice mode is off.", "El modo de voz está desactivado."},
    {"After this I won't hear you until voice mode is turned on again with "
     "the Voice button in the toolbar, or in settings.",
     "Después de esto no te oiré hasta que se vuelva a activar el modo de "
     "voz con el botón Voz de la barra de herramientas, o en la "
     "configuración."},
    {"turn off voice mode", "desactivar el modo de voz"},
    {"go on", "continuar"},
    {"allow it", "permitirlo"},
    {"press {0}", "pulsar {0}"},
    {"send what is in {0}", "enviar lo que hay en {0}"},
    {"Say yes to {0}", "Di sí para {0}"},
    {"Say yes to {0}, or no to cancel.", "Di sí para {0}, o no para cancelar."},
    {"I didn't catch that.", "No te he entendido."},
    {"I can't do that yet: {0}. Say what can I say to hear what I can do.",
     "Todavía no puedo hacer eso: {0}. Di qué puedo decir para saber lo que "
     "puedo hacer."},
    {"Brief answers.", "Respuestas breves."},
    {"Normal detail.", "Detalle normal."},
    {"Full detail.", "Todo el detalle."},
    {"You can say: ", "Puedes decir: "},
    {"; or ", "; o "},
    {"The full list is in Voice commands, in the help.",
     "La lista completa está en Órdenes de voz, en la ayuda."},
    {"That's as fast as I go.", "No puedo hablar más rápido."},
    {"That's as slow as I go.", "No puedo hablar más despacio."},
    {"Faster.", "Más rápido."},
    {"Slower.", "Más despacio."},
    {"Speaking faster.", "Hablo más rápido."},
    {"Speaking slower.", "Hablo más despacio."},

    // --- Going places -----------------------------------------------------
    {"There's no page to go back to.", "No hay ninguna página anterior."},
    {"Going back", "Volviendo atrás"},
    {"There's no page to go forward to.", "No hay ninguna página siguiente."},
    {"Going forward", "Avanzando"},
    {"Reloading", "Recargando"},
    {"Stopped loading.", "He dejado de cargar."},
    {"Going home", "Yendo a la página de inicio"},
    {"New tab.", "Pestaña nueva."},
    {"Opened {0}", "{0} abierto"},
    {"{0} is open.", "{0} está abierto."},
    {"The page is open.", "La página está abierta."},
    {"The page is open, but I can't find a list of results there.",
     "La página está abierta, pero no encuentro una lista de resultados."},
    {"{0} is open, but I can't find a list of results there.",
     "{0} está abierto, pero no encuentro una lista de resultados."},
    {"Which site? Say open, then its name or address.",
     "¿Qué sitio? Di abre y luego su nombre o su dirección."},
    {"Searching for {0}", "Buscando {0}"},
    {"I don't know a site called {0}, so I searched for it.",
     "No conozco ningún sitio llamado {0}, así que lo he buscado."},
    {"Opening {0}", "Abriendo {0}"},
    {"Searching {0} for {1}", "Buscando {1} en {0}"},
    {"I don't know a site called {0}, so I searched the web for {1}.",
     "No conozco ningún sitio llamado {0}, así que he buscado {1} en la "
     "web."},
    {"This page has no search box I can use, so I searched the web.",
     "Esta página no tiene un cuadro de búsqueda que pueda usar, así que he "
     "buscado en la web."},
    {"This site shows more results as you scroll, so I scrolled down. Say "
     "read the results to hear them.",
     "Este sitio muestra más resultados al desplazarte, así que he bajado. "
     "Di lee los resultados para oírlos."},
    {"There's no next page button, so I scrolled down.",
     "No hay ningún botón de página siguiente, así que he bajado."},
    {"There's no such tab.", "No existe esa pestaña."},
    {"Tab {0} of {1}", "Pestaña {0} de {1}"},
    {"More than one tab matches. Say close tab, then its number.",
     "Hay más de una pestaña que coincide. Di cierra la pestaña y luego su "
     "número."},
    {"There's no tab to switch to.",
     "No hay ninguna pestaña a la que cambiar."},
    {"Closed the tab.", "He cerrado la pestaña."},
    {"Closed tab {0}", "He cerrado la pestaña {0}"},

    // --- Viewing ------------------------------------------------------------
    {"Down.", "Abajo."},
    {"Up.", "Arriba."},
    {"Left.", "Izquierda."},
    {"Right.", "Derecha."},
    {"Top of the page.", "Principio de la página."},
    {"Bottom of the page.", "Final de la página."},
    {"Zoomed in.", "Zoom ampliado."},
    {"Zoomed out.", "Zoom reducido."},
    {"Normal size.", "Tamaño normal."},
    {"Numbers on.", "Números activados."},
    {"Numbers off.", "Números desactivados."},

    // --- Media --------------------------------------------------------------
    {"Paused reading", "Lectura en pausa"},
    {"Paused. Say continue reading to go on.",
     "En pausa. Di sigue leyendo para continuar."},
    {"Nothing is playing.", "No se está reproduciendo nada."},
    {"Paused.", "En pausa."},
    {"Setting the volume to a level comes later. Say louder or quieter.",
     "Poner el volumen a un nivel llegará más adelante. Di más alto o más "
     "bajo."},
    {"It's already muted.", "Ya está silenciado."},
    {"It isn't muted.", "No está silenciado."},
    {"Muted.", "Silenciado."},
    {"Sound on.", "Sonido activado."},
    {"Skipped ahead {0}.", "He adelantado {0}."},
    {"Went back {0}.", "He retrocedido {0}."},
    {"From the start.", "Desde el principio."},
    {"Next.", "Siguiente."},
    {"Previous.", "Anterior."},
    {"It isn't full screen.", "No está en pantalla completa."},
    {"Left full screen.", "He salido de la pantalla completa."},
    {"Changing the playback speed comes later.",
     "Cambiar la velocidad de reproducción llegará más adelante."},
    {"Playing: {0}", "Reproduciendo: {0}"},
    {"Playing {0}.", "Reproduciendo {0}."},
    {"It's already playing.", "Ya se está reproduciendo."},
    {"Playing", "Reproduciendo"},
    {"Playing.", "Reproduciendo."},
    {"There's nothing to play on this page.",
     "No hay nada que reproducir en esta página."},
    {"I can't find a volume control on this page. Say mute or unmute.",
     "No encuentro un control de volumen en esta página. Di silencia o "
     "quita el silencio."},
    {"Louder.", "Más alto."},
    {"Quieter.", "Más bajo."},
    {"It's already full screen.", "Ya está en pantalla completa."},
    {"Full screen.", "Pantalla completa."},
    {"There's no skip button now. An ad can often be skipped after a few "
     "seconds: say skip the ad again then.",
     "Ahora no hay botón para saltar. A menudo un anuncio se puede saltar "
     "tras unos segundos: entonces di salta el anuncio otra vez."},
    {"The ad can't be skipped yet.", "Todavía no se puede saltar el anuncio."},
    {"Skipping the ad", "Saltando el anuncio"},
    {"Skipped the ad.", "He saltado el anuncio."},
    {"I can't find a captions button on this page.",
     "No encuentro un botón de subtítulos en esta página."},
    {"Captions are already on.", "Los subtítulos ya están activados."},
    {"Captions are already off.", "Los subtítulos ya están desactivados."},
    {"Captions on.", "Subtítulos activados."},
    {"Captions off.", "Subtítulos desactivados."},

    // --- Reading ------------------------------------------------------------
    {"There's no list of results on this page.",
     "No hay ninguna lista de resultados en esta página."},
    {"This page has no title.", "Esta página no tiene título."},
    {"The title is: {0}.", "El título es: {0}."},
    {"Which section? Say read the section on, then its name.",
     "¿Qué sección? Di lee la sección sobre y luego su nombre."},
    {"Reading the page", "Leyendo la página"},
    {"Reading the section on {0}", "Leyendo la sección sobre {0}"},
    {"Reading the selection", "Leyendo la selección"},
    {"I can only answer questions about web pages.",
     "Solo puedo responder preguntas sobre páginas web."},
    {"Looking for more on the page", "Buscando más en la página"},
    {"Looking at the page", "Mirando la página"},
    {"Reading the results", "Leyendo los resultados"},
    {"Results for {0}", "Resultados de {0}"},
    {"result", "resultado"},
    {"results", "resultados"},
    {"This page has no headings.", "Esta página no tiene títulos."},
    {"heading", "título"},
    {"headings", "títulos"},
    {"This page has no links.", "Esta página no tiene enlaces."},
    {"link", "enlace"},
    {"links", "enlaces"},
    {"item", "elemento"},
    {"items", "elementos"},
    {"a video", "un vídeo"},
    {"an audio player", "un reproductor de audio"},
    {"a list of {0}", "una lista de {0}"},
    {"a search box", "un cuadro de búsqueda"},
    {"text field", "campo de texto"},
    {"text fields", "campos de texto"},
    {"This page", "Esta página"},
    {"{0}. I can't find anything to read or choose on it.",
     "{0}. No encuentro nada que leer ni elegir en ella."},
    {". On it: ", ". En ella: "},
    {" and ", " y "},
    {"You're on {0}", "Estás en {0}"},
    {"You're on a browser page.", "Estás en una página del navegador."},
    {"You're on {0}.", "Estás en {0}."},
    {" Tab {0} of {1}.", " Pestaña {0} de {1}."},
    {"Find what? Say find, then the words.",
     "¿Buscar qué? Di busca y luego las palabras, en esta página."},
    {"the heading {0}", "el título {0}"},
    {"the link", "el enlace"},
    {"the button", "el botón"},
    {"the text field", "el campo de texto"},
    {"the check box", "la casilla"},
    {"the radio button", "el botón de opción"},
    {"the switch", "el interruptor"},
    {"the menu", "el menú"},
    {"the option", "la opción"},
    {"the menu item", "el elemento de menú"},
    {"the tab", "la pestaña"},
    {"I can't find {0} in this page's headings, links or buttons.",
     "No encuentro {0} en los títulos, enlaces ni botones de esta página."},
    {"Found it in {0}.", "Lo he encontrado en {0}."},
    {" It's in {0} places.", " Aparece en {0} sitios."},
    {"That's the last link.", "Ese es el último enlace."},
    {"That's the first link.", "Ese es el primer enlace."},
    {"Link {0}: {1}.", "Enlace {0}: {1}."},
    {"There's no list to move through. Say read the results.",
     "No hay ninguna lista que recorrer. Di lee los resultados."},
    {"That's the last one.", "Ese es el último."},
    {"That's the first one.", "Ese es el primero."},

    // --- Choosing -----------------------------------------------------------
    {"The page changed since I asked. Say it again.",
     "La página ha cambiado desde que pregunté. Dilo otra vez."},
    {"Which one?", "¿Cuál?"},
    {"There's nothing here to choose from.", "Aquí no hay nada que elegir."},
    {"There's no number {0} here. {1}", "Aquí no hay ningún número {0}. {1}"},
    {"Choose one first: say a number or a name.",
     "Elige uno primero: di un número o un nombre."},
    {"Nothing here matches {0}. {1}", "Nada de aquí coincide con {0}. {1}"},
    {"Heading: {0}.", "Título: {0}."},
    {"{0}. There's nothing to open there.", "{0}. Ahí no hay nada que abrir."},
    {"{0}, {1} field. Voice mode never types or reads passwords, card "
     "numbers or codes.",
     "{0}, campo de {1}. El modo de voz nunca escribe ni lee contraseñas, "
     "números de tarjeta ni códigos."},
    {"password", "contraseña"},
    {"payment card", "tarjeta de pago"},
    {"one-time code", "código de un solo uso"},
    {"Text field.", "Campo de texto."},
    {"{0} field.", "Campo {0}."},
    {" It says: {0}.", " Dice: {0}."},
    {" Say type, then your text.", " Di escribe y luego tu texto."},
    {"{0} slider", "Control deslizante {0}"},
    {"Opening to play: {0}", "Abriendo para reproducir: {0}"},
    {"Opening: {0}", "Abriendo: {0}"},
    {"Pressed {0}.", "He pulsado {0}."},
    {"Pressing: {0}", "Pulsando: {0}"},

    // --- Typing -------------------------------------------------------------
    {"Choose a field first: say its name or its number.",
     "Elige un campo primero: di su nombre o su número."},
    {"The field", "El campo"},
    {"{0} is a {1} field. Voice mode never types or reads passwords, card "
     "numbers or codes.",
     "{0} es un campo de {1}. El modo de voz nunca escribe ni lee "
     "contraseñas, números de tarjeta ni códigos."},
    {"{0} is empty.", "{0} está vacío."},
    {"{0} says: {1}.", "{0} dice: {1}."},
    {"I can't type in {0}.", "No puedo escribir en {0}."},
    {"Typed.", "Escrito."},
    {"New line.", "Línea nueva."},
    {"Deleted {0}.", "He borrado {0}."},
    {"Cleared.", "Borrado."},
    {"{0} already holds long text. Say clear the field, then type it again.",
     "{0} ya tiene un texto largo. Di borra el campo y luego escríbelo otra "
     "vez."},

    // --- When a step fails (FailureText()) ----------------------------------
    {"Voice mode is off in this tab.",
     "El modo de voz está desactivado en esta pestaña."},
    {"The page changed before I could do that. Say it again.",
     "La página cambió antes de que pudiera hacerlo. Dilo otra vez."},
    {"I can't do that on this page.", "No puedo hacer eso en esta página."},
    {"That's turned off on the page.", "Eso está desactivado en la página."},
    {"I can't type there.", "No puedo escribir ahí."},
    {"That's a password, card or code field. Voice mode never types or reads "
     "those.",
     "Ese es un campo de contraseña, tarjeta o código. El modo de voz nunca "
     "los escribe ni los lee."},
    {"That's too much text to type.", "Es demasiado texto para escribir."},
    {"That isn't something I can play.",
     "Eso no es algo que pueda reproducir."},
    {"Nothing has played in this tab yet.",
     "Todavía no se ha reproducido nada en esta pestaña."},
    {"The page didn't let me send that.", "La página no me dejó enviarlo."},
    {"I can only open web pages.", "Solo puedo abrir páginas web."},
    {"There's no tab to do that in.", "No hay ninguna pestaña donde hacerlo."},
    {"I can't do that here.", "No puedo hacer eso aquí."},
};

}  // namespace

std::vector<SpokenAnswer> MakeSpanishSpokenAnswers() {
  return std::vector<SpokenAnswer>(std::begin(kAnswers), std::end(kAnswers));
}

}  // namespace voice
}  // namespace molt_ai
