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
    // A routine's confirmations (la rutina: feminine).
    {"replace it", "reemplazarla"},
    {"keep it here", "conservarla aquí"},
    {"keep them here", "conservarlas aquí"},
    {"{0} routines were made before routines belonged to a profile. Choose "
     "the ones to keep here in Agent mode.",
     "{0} rutinas se crearon antes de que las rutinas pertenecieran a un "
     "perfil. Elige en el modo agente las que quieras conservar aquí."},
    {"One routine was made before routines belonged to a profile: {0}.",
     "Una rutina se creó antes de que las rutinas pertenecieran a un "
     "perfil: {0}."},
    {"{0} routines were made before routines belonged to a profile: {1}.",
     "{0} rutinas se crearon antes de que las rutinas pertenecieran a un "
     "perfil: {1}."},
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

    // --- The browser's prompts and a page's dialogs (voice_prompts.cc,
    // voice_browser_prompts.cc). The answers quoted are the ones
    // ParsePromptReply() takes in Spanish; "la palabra" (the confirmation
    // word) is feminine, a permission or a file is "lo", the sites "los".
    {"Permission request", "Solicitud de permiso"},
    {"Waiting for your answer", "Esperando tu respuesta"},
    {"Checking the word", "Comprobando la palabra"},
    {"the word on your screen", "la palabra que ves en la pantalla"},
    {"I don't say the word aloud while another program could hear it.",
     "No digo la palabra en voz alta mientras otro programa pueda oírla."},
    {"{0} is asking for a permission.", "{0} pide un permiso."},
    {"{0} wants to {1}.", "{0} quiere {1}."},
    {"To allow it, say {0}. To block it, say no.",
     "Para permitirlo, di {0}. Para bloquearlo, di no."},
    {"I can't take the word while your screen is shared, so to allow it, "
     "use the mouse or keyboard. To block it, say no.",
     "No puedo aceptar la palabra mientras compartes la pantalla, así que "
     "para permitirlo usa el ratón o el teclado. Para bloquearlo, di no."},
    {"To block it, say no. To allow it, use the mouse or keyboard.",
     "Para bloquearlo, di no. Para permitirlo, usa el ratón o el teclado."},
    {"{0} shows another message. Say OK to close it.",
     "{0} muestra otro mensaje. Di vale para cerrarlo."},
    {"{0} shows a message. Say OK to close it.",
     "{0} muestra un mensaje. Di vale para cerrarlo."},
    {"{0} says: {1} Say OK to close it.",
     "{0} dice: {1} Di vale para cerrarlo."},
    {"{0} asks another question. Say yes or no.",
     "{0} hace otra pregunta. Di sí o no."},
    {"{0} asks you to confirm. Say yes or no.",
     "{0} te pide que confirmes. Di sí o no."},
    {"{0} asks: {1} Say yes or no.", "{0} pregunta: {1} Di sí o no."},
    {"{0} asks for an answer. Say answer, then your answer, or say no to "
     "cancel.",
     "{0} pide una respuesta. Di responde y luego tu respuesta, o di no "
     "para cancelar."},
    {"{0} asks: {1} Say answer, then your answer, or say no to cancel.",
     "{0} pregunta: {1} Di responde y luego tu respuesta, o di no para "
     "cancelar."},
    {"Reload {0}? Changes you made may not be saved. Say reload or stay.",
     "¿Recargar {0}? Puede que no se guarden los cambios que hiciste. Di "
     "recarga o quédate."},
    {"Leave {0}? Changes you made may not be saved. Say leave or stay.",
     "¿Salir de {0}? Puede que no se guarden los cambios que hiciste. Di "
     "sal o quédate."},
    {"A file from {0} is dangerous, so it was blocked. To delete it, say "
     "discard. To keep it anyway, use the downloads page.",
     "Un archivo de {0} es peligroso, así que se ha bloqueado. Para "
     "borrarlo, di descarta. Para conservarlo de todos modos, usa la página "
     "de descargas."},
    {"The file {0} from {1} is dangerous, so it was blocked. To delete it, "
     "say discard. To keep it anyway, use the downloads page.",
     "El archivo {0} de {1} es peligroso, así que se ha bloqueado. Para "
     "borrarlo, di descarta. Para conservarlo de todos modos, usa la página "
     "de descargas."},
    {"A file from {0} may be harmful.", "Un archivo de {0} puede ser dañino."},
    {"The file {0} from {1} may be harmful.",
     "El archivo {0} de {1} puede ser dañino."},
    {"To keep it, say {0}. To delete it, say discard.",
     "Para conservarlo, di {0}. Para borrarlo, di descarta."},
    {"I can't take the word while your screen is shared. To delete it, say "
     "discard. To keep it, use the downloads page.",
     "No puedo aceptar la palabra mientras compartes la pantalla. Para "
     "borrarlo, di descarta. Para conservarlo, usa la página de "
     "descargas."},
    {"To delete it, say discard. To keep it, use the downloads page.",
     "Para borrarlo, di descarta. Para conservarlo, usa la página de "
     "descargas."},
    {"Save your password for {0}? Say save, never, or not now.",
     "¿Guardar tu contraseña de {0}? Di guarda, nunca o ahora no."},
    {"Confirm the sites this routine uses?",
     "¿Confirmas los sitios que usa esta rutina?"},
    {"Confirm the sites this routine uses: {0}?",
     "¿Confirmas los sitios que usa esta rutina: {0}?"},
    {"To confirm them, say {0}. To leave them unconfirmed, say no.",
     "Para confirmarlos, di {0}. Para dejarlos sin confirmar, di no."},
    {"I can't take the word while your screen is shared. Confirm them in "
     "the Agent studio, or say no.",
     "No puedo aceptar la palabra mientras compartes la pantalla. "
     "Confírmalos en el estudio del modo agente, o di no."},
    {"Confirm them in the Agent studio, or say no.",
     "Confírmalos en el estudio del modo agente, o di no."},
    {"No answer, so I dismissed the permission request.",
     "No has respondido, así que he descartado la solicitud de permiso."},
    {"No answer, so the file stays blocked. You can decide on the downloads "
     "page.",
     "No has respondido, así que el archivo sigue bloqueado. Puedes decidir "
     "en la página de descargas."},
    {"No answer, so the sites stay unconfirmed.",
     "No has respondido, así que los sitios siguen sin confirmar."},
    {"I couldn't be sure that was you. When it's quiet and I've finished "
     "speaking, say {0}.",
     "No he podido asegurarme de que fueras tú. Cuando haya silencio y yo "
     "haya terminado de hablar, di {0}."},
    {"I couldn't be sure that was you, and something else may have heard "
     "the word, so I changed it.",
     "No he podido asegurarme de que fueras tú, y puede que otra cosa haya "
     "oído la palabra, así que la he cambiado."},
    {"Something else may have heard you say the word, so I changed it.",
     "Puede que otra cosa te haya oído decir la palabra, así que la he "
     "cambiado."},
    {"Something started using a microphone, so I changed the word.",
     "Algo ha empezado a usar un micrófono, así que he cambiado la "
     "palabra."},
    {"Your screen is no longer shared.", "Ya no compartes la pantalla."},
    {"That word changed. To keep it, say {0}.",
     "Esa palabra ha cambiado. Para conservarlo, di {0}."},
    {"That word changed. To confirm them, say {0}.",
     "Esa palabra ha cambiado. Para confirmarlos, di {0}."},
    {"That word changed. To allow it, say {0}.",
     "Esa palabra ha cambiado. Para permitirlo, di {0}."},
    {"Kept the file.", "He conservado el archivo."},
    {"Confirmed.", "Confirmado."},
    {"Allowed.", "Permitido."},
    {"That wasn't the word, so I didn't keep the file.",
     "Esa no era la palabra, así que no he conservado el archivo."},
    {"That wasn't the word, so I turned it down.",
     "Esa no era la palabra, así que lo he rechazado."},
    {"Deleted the file.", "He borrado el archivo."},
    {"I won't offer to save passwords for this site.",
     "No volveré a ofrecerte guardar contraseñas de este sitio."},
    {"Blocked.", "Bloqueado."},
    {"Closed.", "Cerrado."},
    {"Yes.", "Sí."},
    {"Sent.", "Enviado."},
    {"Leaving.", "Saliendo."},
    {"Saved.", "Guardado."},
    {"No.", "No."},
    {"Staying on the page.", "Me quedo en la página."},
    {"Not now.", "Ahora no."},
    {"Dismissed.", "Descartado."},
    {"Left unconfirmed.", "Sin confirmar."},
    {"Your answer: {0}. Say yes to send it, or no to cancel.",
     "Tu respuesta: {0}. Di sí para enviarla, o no para cancelar."},
    {"To keep it, say {0}.", "Para conservarlo, di {0}."},
    {"To confirm them, say {0}.", "Para confirmarlos, di {0}."},
    {"To allow it, say {0}.", "Para permitirlo, di {0}."},
    {"I didn't catch that. Say the word again.",
     "No te he entendido. Di la palabra otra vez."},
    {"I can't keep a dangerous file by voice. Use the downloads page, or say "
     "discard.",
     "No puedo conservar un archivo peligroso por voz. Usa la página de "
     "descargas, o di descarta."},
    {"I can't take the word while your screen is shared. Use the downloads "
     "page, or say discard.",
     "No puedo aceptar la palabra mientras compartes la pantalla. Usa la "
     "página de descargas, o di descarta."},
    {"I can't take the word while your screen is shared. Use the mouse or "
     "keyboard, or say no.",
     "No puedo aceptar la palabra mientras compartes la pantalla. Usa el "
     "ratón o el teclado, o di no."},
    {"I can't keep it by voice right now. Use the downloads page, or say "
     "discard.",
     "Ahora no puedo conservarlo por voz. Usa la página de descargas, o di "
     "descarta."},
    {"I can't allow that by voice right now. Use the mouse or keyboard, or "
     "say no.",
     "Ahora no puedo permitirlo por voz. Usa el ratón o el teclado, o di "
     "no."},
    {"I can't take a word for this one any more.",
     "Ya no puedo aceptar una palabra para esto."},
    {"From now on, the word for this one is only on your screen.",
     "A partir de ahora, la palabra para esto solo está en tu pantalla."},
    {"I couldn't confirm them.", "No he podido confirmarlos."},
    {"That's twice: for the next ten minutes, allow things with the mouse or "
     "keyboard.",
     "Ya van dos veces: durante los próximos diez minutos, permite las cosas "
     "con el ratón o el teclado."},
    {"I can't take the word now. Use the mouse or keyboard.",
     "Ahora no puedo aceptar la palabra. Usa el ratón o el teclado."},
    {"I couldn't allow it.", "No he podido permitirlo."},
    {"Allowed for this visit.", "Permitido durante esta visita."},
    {"I couldn't keep it. You can decide on the downloads page.",
     "No he podido conservarlo. Puedes decidir en la página de descargas."},
    // What a permission prompt asks (PermissionPhrase()): "{0} quiere ...".
    {"use your camera", "usar tu cámara"},
    {"use your microphone", "usar tu micrófono"},
    {"know your location", "saber tu ubicación"},
    {"show notifications", "mostrar notificaciones"},
    {"see what you copy to the clipboard",
     "ver lo que copias en el portapapeles"},
    {"control your MIDI devices", "controlar tus dispositivos MIDI"},
    {"use its cookies on other sites", "usar sus cookies en otros sitios"},
    {"download several files", "descargar varios archivos"},
    {"reach devices on your local network",
     "acceder a dispositivos de tu red local"},
    {"use your motion sensors", "usar tus sensores de movimiento"},
    {"manage windows on all your screens",
     "gestionar ventanas en todas tus pantallas"},
    {"edit files", "editar archivos"},
    {"know when you're using your device",
     "saber cuándo estás usando tu dispositivo"},

    // --- The listening session (voice_session.cc) ---------------------------
    {"To interrupt me, say stop, then what you want.",
     "Para interrumpirme, di para y luego lo que quieras."},
    {"I've stopped. Now say that again.",
     "Me he detenido. Ahora dilo otra vez."},
    {"I can't use the microphone: the system's privacy settings don't let "
     "MoltBrowser use it. Allow it there, then turn voice mode on again.",
     "No puedo usar el micrófono: la configuración de privacidad del sistema "
     "no deja que MoltBrowser lo use. Permítelo ahí y vuelve a activar el "
     "modo de voz."},
    {"I can't find a microphone. Connect one, then turn voice mode on again.",
     "No encuentro ningún micrófono. Conecta uno y vuelve a activar el modo "
     "de voz."},
    {"The microphone stopped working. Turn voice mode on again to try once "
     "more.",
     "El micrófono ha dejado de funcionar. Vuelve a activar el modo de voz "
     "para intentarlo otra vez."},
    {"I can't use the microphone.", "No puedo usar el micrófono."},
    {"I can't listen: {0}.", "No puedo escuchar: {0}."},
    {"Voice mode is on. I'm listening.",
     "El modo de voz está activado. Te escucho."},
    {"Voice mode is on. Say Hey Molt, then what you want.",
     "El modo de voz está activado. Di Oye Molt y luego lo que quieras."},
    {"Speech recognition is restarting",
     "El reconocimiento de voz se está reiniciando"},
    {"Speech recognition stopped working: {0}. Turn voice mode on again with "
     "the Voice button.",
     "El reconocimiento de voz ha dejado de funcionar: {0}. Vuelve a activar "
     "el modo de voz con el botón Voz."},
    // Why the speech helper can't run (voice_listener.cc), said in the
    // sentences above.
    {"voice mode's speech recognition isn't installed",
     "el reconocimiento de voz del modo de voz no está instalado"},
    {"voice mode's speech models aren't installed",
     "los modelos de voz del modo de voz no están instalados"},
    {"its sandbox couldn't be set up", "no se pudo preparar su aislamiento"},
    {"it stopped answering", "dejó de responder"},
    {"it crashed", "se cerró de forma inesperada"},
    {"it wrote too many lines", "escribió demasiadas líneas"},
    {"it wrote a line longer than the protocol allows",
     "escribió una línea más larga de lo que permite el protocolo"},
    {"it stopped reading its input", "dejó de leer su entrada"},
    {"The page changed, so its numbers moved. Say the number you see now.",
     "La página ha cambiado, así que sus números se han movido. Di el número "
     "que ves ahora."},
    {"I didn't hear yes, so I stopped.",
     "No he oído un sí, así que he parado."},

    // --- Routines and learning (voice_controller_logic.cc,
    // voice_learning_loop.cc, voice_routines*.cc). A routine is "la
    // rutina"; the commands quoted are the Spanish grammar's.
    {"I can't keep routines here.", "Aquí no puedo guardar rutinas."},
    {"Say remember this as, then a short name for it.",
     "Di recuerda esto como y luego un nombre corto para ello."},
    {"You have {0} routines called {1}. Choose another name, or rename them "
     "in Agent mode.",
     "Tienes {0} rutinas llamadas {1}. Elige otro nombre, o cámbiales el "
     "nombre en el modo agente."},
    {"There's nothing to remember yet. Do it once, then say remember this as "
     "{0}.",
     "Todavía no hay nada que recordar. Hazlo una vez y luego di recuerda "
     "esto como {0}."},
    {"I can't remember this: {0}", "No puedo recordar esto: {0}"},
    {"Remembering {0}", "Recordando {0}"},
    {"You already have a routine called {0}.",
     "Ya tienes una rutina llamada {0}."},
    {"I'll remember: {0}.", "Recordaré: {0}."},
    {"Some of these steps are the AI's, so you'll confirm the sites they use "
     "before it runs.",
     "Algunos de estos pasos son de la IA, así que confirmarás los sitios "
     "que usan antes de que se ejecute."},
    {"save it as {0}", "guardarla como {0}"},
    {"I can't run routines here.", "Aquí no puedo ejecutar rutinas."},
    {"You have {0} routines called {1}. Rename one in Agent mode to run it by "
     "voice.",
     "Tienes {0} rutinas llamadas {1}. Cámbiale el nombre a una en el modo "
     "agente para ejecutarla por voz."},
    {"{0} was made before routines belonged to a profile.",
     "{0} se creó antes de que las rutinas pertenecieran a un perfil."},
    {"You don't have any routines yet. Do something, then say remember this "
     "as, and a name.",
     "Todavía no tienes rutinas. Haz algo y luego di recuerda esto como y un "
     "nombre."},
    {"I don't have a routine called {0}. Say what are my routines.",
     "No tengo ninguna rutina llamada {0}. Di cuáles son mis rutinas."},
    {"Running {0}", "Ejecutando {0}"},
    {"You have {0} routines called {1}. Forget the one you mean in Agent "
     "mode.",
     "Tienes {0} rutinas llamadas {1}. Olvida la que quieras en el modo "
     "agente."},
    {"I don't have a routine called {0}.",
     "No tengo ninguna rutina llamada {0}."},
    {"forget the routine {0}", "olvidar la rutina {0}"},
    {"You have one routine: {0}. Say run, then its name.",
     "Tienes una rutina: {0}. Di ejecuta y luego su nombre."},
    {"You have {0} routines: {1}, and more in Agent mode. Say run, then its "
     "name.",
     "Tienes {0} rutinas: {1}, y más en el modo agente. Di ejecuta y luego "
     "su nombre."},
    {"You have {0} routines: {1}. Say run, then its name.",
     "Tienes {0} rutinas: {1}. Di ejecuta y luego su nombre."},
    {"Okay, I won't replace it. Say remember this as, and another name.",
     "Vale, no la reemplazaré. Di recuerda esto como y otro nombre."},
    {"Okay, I'll leave them for another profile. You can still keep them "
     "here in Agent mode.",
     "Vale, las dejo para otro perfil. Todavía puedes conservarlas aquí en el "
     "modo agente."},
    {"I haven't learned the word {0}.", "No he aprendido la palabra {0}."},
    {"forget the word {0}", "olvidar la palabra {0}"},
    {"That's your routines, the words and corrections I learned, and the "
     "site hints.",
     "Eso son tus rutinas, las palabras y correcciones que aprendí y las "
     "pistas de los sitios."},
    {"forget everything I learned from you",
     "olvidar todo lo que aprendí de ti"},
    {"I couldn't find it on this page.", "No lo he encontrado en esta página."},
    {"I couldn't find {0} on this page.",
     "No he encontrado {0} en esta página."},
    {"I found more than one it on this page.",
     "He encontrado más de uno en esta página."},
    {"I found more than one {0} on this page.",
     "He encontrado más de un {0} en esta página."},
    {"That step didn't work.", "Ese paso no ha funcionado."},
    {"I stopped {0} at step {1}.", "He parado {0} en el paso {1}."},
    {"I stopped {0}.", "He parado {0}."},
    {"I forgot {0}.", "He olvidado {0}."},
    {"I couldn't forget {0}.", "No he podido olvidar {0}."},
    {"I forgot the word {0}.", "He olvidado la palabra {0}."},
    {"Done. I forgot everything I learned from you.",
     "Hecho. He olvidado todo lo que aprendí de ti."},
    {"What I'd remember changed. Say remember this again.",
     "Lo que iba a recordar ha cambiado. Di recuerda esto otra vez."},
    {"Replaced {0}. Say run {0} to do it again.",
     "He reemplazado {0}. Di ejecuta {0} para hacerlo otra vez."},
    {"Saved. Say run {0} to do it again.",
     "Guardada. Di ejecuta {0} para hacerlo otra vez."},
    {"Replaced {0}. It uses {1}. Its sites need your confirmation the first "
     "time you run it.",
     "He reemplazado {0}. Usa {1}. Sus sitios necesitan tu confirmación la "
     "primera vez que la ejecutes."},
    {"Saved {0}. It uses {1}. Its sites need your confirmation the first "
     "time you run it.",
     "He guardado {0}. Usa {1}. Sus sitios necesitan tu confirmación la "
     "primera vez que la ejecutes."},
    {"Those routines changed since I asked. Say what are my routines again.",
     "Esas rutinas han cambiado desde que pregunté. Di cuáles son mis rutinas "
     "otra vez."},
    {"I couldn't keep them here. Another profile may have kept them first.",
     "No he podido conservarlas aquí. Puede que otro perfil las conservara "
     "antes."},
    {"Kept it here.", "La he conservado aquí."},
    {"Kept {0} routines here.", "He conservado {0} rutinas aquí."},
    {"You already had a routine called {0}, so this one is {1}.",
     "Ya tenías una rutina llamada {0}, así que esta se llama {1}."},
    {"{0} wasn't there any more.", "{0} ya no estaba."},
    {"{0} weren't there any more.", "{0} ya no estaban."},
    {"I can't find the routine {0}.", "No encuentro la rutina {0}."},
    {"Before {0} runs, its sites need your confirmation. {1}",
     "Antes de ejecutar {0}, sus sitios necesitan tu confirmación. {1}"},
    {"I couldn't confirm its sites.", "No he podido confirmar sus sitios."},
    {"Confirm it in the Agent studio for now.",
     "Por ahora, confírmalo en el estudio del modo agente."},
    {"{0} is done.", "{0} ha terminado."},
    {"Running {0}. Step {1}: {2}.", "Ejecutando {0}. Paso {1}: {2}."},
    {"Step {0}: {1}.", "Paso {0}: {1}."},
    {"I didn't keep the text for it. Say type, then the text, or anything "
     "else to stop.",
     "No guardé el texto para eso. Di escribe y luego el texto, o cualquier "
     "otra cosa para parar."},
    {"I stopped {0}: step {1} would act on {2}, which isn't one of its "
     "sites.",
     "He parado {0}: el paso {1} actuaría en {2}, que no es uno de sus "
     "sitios."},
    {"I stopped {0}: step {1} would open {2}, which isn't one of its sites.",
     "He parado {0}: el paso {1} abriría {2}, que no es uno de sus sitios."},
    {"I can't run the AI's steps here, so I stopped {0}.",
     "Aquí no puedo ejecutar los pasos de la IA, así que he parado {0}."},
    {"I didn't hear an answer, so I stopped {0}.",
     "No he oído una respuesta, así que he parado {0}."},
    {"There's nothing to remember yet.", "Todavía no hay nada que recordar."},
    {"That's too many steps for one routine.",
     "Son demasiados pasos para una rutina."},
    {"Choose another name for it.", "Elige otro nombre para ella."},
    {"One of the steps can't be kept.",
     "Uno de los pasos no se puede guardar."},
    {"One of the steps holds text I can't keep.",
     "Uno de los pasos tiene un texto que no puedo guardar."},
    {"One of the pages looks like it holds a password or a code, so I won't "
     "remember it.",
     "Una de las páginas parece contener una contraseña o un código, así que "
     "no la recordaré."},
    {"One of the choices looks like a code, so I won't remember it.",
     "Una de las elecciones parece un código, así que no la recordaré."},
    {"This routine has no steps.", "Esta rutina no tiene pasos."},
    {"This routine is too long to run by voice.",
     "Esta rutina es demasiado larga para ejecutarla por voz."},
    {"Step {0} of this routine isn't one I can do by voice.",
     "El paso {0} de esta rutina no es uno que pueda hacer por voz."},
    {"I couldn't save it.", "No he podido guardarla."},
    // Why a step of the AI's can't be kept (said after "No puedo recordar
    // esto: ").
    {"one of the AI's steps can't be repeated.",
     "uno de los pasos de la IA no se puede repetir."},
    {"the AI's steps aren't kept while Learn from what I say is off.",
     "los pasos de la IA no se guardan mientras Aprender de lo que digo está "
     "desactivado."},
    {"a page the AI opened looks like it holds a password or a code.",
     "una página que abrió la IA parece contener una contraseña o un "
     "código."},
    {"a search the AI made looks like it holds a password or a code.",
     "una búsqueda que hizo la IA parece contener una contraseña o un "
     "código."},
    {"the AI typed into a password, card or code field, which I never keep.",
     "la IA escribió en un campo de contraseña, tarjeta o código, que nunca "
     "guardo."},
    {"the AI used something on the page without a name, which I couldn't "
     "find again.",
     "la IA usó algo de la página sin nombre, que no podría volver a "
     "encontrar."},
    // A routine's steps as they are said ("Paso 1: abre YouTube"), as the
    // Spanish grammar's commands.
    {"what you tell me", "lo que me digas"},
    {"open {0} in a new tab", "abre {0} en una pestaña nueva"},
    {"open {0}", "abre {0}"},
    {"search {0} for {1}", "busca {1} en {0}"},
    {"search the web for {0}", "busca {0} en la web"},
    {"search this site for {0}", "busca {0} en este sitio"},
    {"search for {0}", "busca {0}"},
    {"find {0}", "busca {0} en esta página"},
    {"go back", "atrás"},
    {"go forward", "adelante"},
    {"reload the page", "recarga la página"},
    {"go home", "ve a inicio"},
    {"go to the next page", "ve a la página siguiente"},
    {"scroll down", "baja"},
    {"scroll up", "sube"},
    {"scroll left", "desplázate a la izquierda"},
    {"scroll right", "desplázate a la derecha"},
    {"scroll down {0} pages", "baja {0} páginas"},
    {"scroll up {0} pages", "sube {0} páginas"},
    {"scroll left {0} pages", "desplázate {0} páginas a la izquierda"},
    {"scroll right {0} pages", "desplázate {0} páginas a la derecha"},
    {"scroll down a bit", "baja un poco"},
    {"scroll up a bit", "sube un poco"},
    {"scroll left a bit", "desplázate un poco a la izquierda"},
    {"scroll right a bit", "desplázate un poco a la derecha"},
    {"go to the top", "ve al principio"},
    {"go to the bottom", "ve al final"},
    {"zoom in", "acerca"},
    {"zoom out", "aleja"},
    {"reset the zoom", "restablece el zoom"},
    {"play", "reproduce"},
    {"pause", "pausa"},
    {"resume", "reanuda"},
    {"louder", "sube el volumen"},
    {"quieter", "baja el volumen"},
    {"mute", "silencia"},
    {"unmute", "activa el sonido"},
    {"skip ahead {0}", "adelanta {0}"},
    {"skip ahead", "adelanta"},
    {"skip back {0}", "retrocede {0}"},
    {"skip back", "retrocede"},
    {"play from the start", "empieza de nuevo"},
    {"next track", "siguiente pista"},
    {"previous track", "pista anterior"},
    {"full screen", "pantalla completa"},
    {"leave full screen", "sal de la pantalla completa"},
    {"turn on captions", "activa los subtítulos"},
    {"turn off captions", "quita los subtítulos"},
    {"read the page", "lee la página"},
    {"read the headings", "lee los títulos"},
    {"read the links", "lee los enlaces"},
    {"read the results", "lee los resultados"},
    {"read the title", "lee el título"},
    {"say what's on the screen", "di qué hay en la pantalla"},
    {"type {0}", "escribe {0}"},
    {"start a new line", "nueva línea"},
    {"clear the field", "borra el campo"},
    {"submit", "envía"},
    {"choose {0}", "elige {0}"},
    {"play {0}", "reproduce {0}"},
    {"click {0}", "pulsa {0}"},
    {"select {0}", "selecciona {0}"},
    {"read {0}", "lee {0}"},
    {"describe {0}", "describe {0}"},
    {"the last one", "el último"},
    {"number {0} from the end", "el número {0} desde el final"},
    {"the {0} to last one", "el {0} desde el final"},
    {"number {0}", "el número {0}"},
    {"the {0} one", "el {0}"},
    {"one", "uno"},
    {"open a page", "abre una página"},
    {"search the web", "busca en la web"},
    {"type {0} in {1}", "escribe {0} en {1}"},
    {"type what you tell me in {0}", "escribe lo que me digas en {0}"},
    {"choose {0} in {1}", "elige {0} en {1}"},
    {"a step the AI took that I can't repeat",
     "un paso de la IA que no puedo repetir"},
    {"a step the AI took", "un paso de la IA"},

    // --- What the web agent does for voice mode (voice_agent_speech.cc) -----
    // The plan's steps and the agent's results are the model's (in
    // English until the planner is asked in Spanish).
    {"Working on it.", "Me pongo con ello."},
    {"Next, I'll {0}.", "Ahora voy a: {0}."},
    {"I'll {0}.", "Voy a: {0}."},
    {"That didn't work, so I'm trying another way.",
     "Eso no ha funcionado, así que lo intento de otra forma."},
    {"That didn't work, so I'll {0} instead.",
     "Eso no ha funcionado, así que en su lugar voy a: {0}."},
    {"Opening the page.", "Abriendo la página."},
    {"Opening {0}.", "Abriendo {0}."},
    {"Searching the web for {0}.", "Buscando {0} en la web."},
    {"Pressing it.", "Pulsándolo."},
    {"Pressing {0}.", "Pulsando {0}."},
    {"Clearing the field.", "Borrando el campo."},
    {"Clearing the field in {0}.", "Borrando el campo {0}."},
    {"Typing {0}.", "Escribiendo {0}."},
    {"Typing {0} in {1}.", "Escribiendo {0} en {1}."},
    {"Choosing {0}.", "Eligiendo {0}."},
    {"Choosing {0} in {1}.", "Eligiendo {0} en {1}."},
    {"Filling in the form.", "Rellenando el formulario."},
    {"Scrolling up.", "Subiendo."},
    {"Scrolling down.", "Bajando."},
    {"Using a tool of the page.", "Usando una herramienta de la página."},
    {"Using the page's {0} tool.", "Usando la herramienta {0} de la página."},
    {"Done.", "Hecho."},
    {"Done. {0}.", "Hecho. {0}."},
    {"I stopped. Say what can I say to hear the commands I know.",
     "Me he detenido. Di qué puedo decir para oír las órdenes que conozco."},
    {"I stopped: {0}. Say what can I say to hear the commands I know.",
     "Me he detenido: {0}. Di qué puedo decir para oír las órdenes que "
     "conozco."},
    {"I can't do that by myself yet: no on-device AI model is installed. You "
     "can add one in the AI settings. Say what can I say to hear the "
     "commands I know.",
     "Todavía no puedo hacer eso yo solo: no hay ningún modelo de IA "
     "instalado en este dispositivo. Puedes añadir uno en la configuración "
     "de IA. Di qué puedo decir para oír las órdenes que conozco."},
    {"The on-device AI can't run on this computer. Say what can I say to "
     "hear the commands I know.",
     "La IA del dispositivo no puede funcionar en este ordenador. Di qué "
     "puedo decir para oír las órdenes que conozco."},
    {"The on-device AI can't run on this computer: {0}. Say what can I say "
     "to hear the commands I know.",
     "La IA del dispositivo no puede funcionar en este ordenador: {0}. Di "
     "qué puedo decir para oír las órdenes que conozco."},
    {"I couldn't start the on-device AI. Say what can I say to hear the "
     "commands I know.",
     "No he podido iniciar la IA del dispositivo. Di qué puedo decir para "
     "oír las órdenes que conozco."},
    {"I couldn't start the on-device AI: {0}. Say what can I say to hear the "
     "commands I know.",
     "No he podido iniciar la IA del dispositivo: {0}. Di qué puedo decir "
     "para oír las órdenes que conozco."},
    {"There's no tab for me to work in. Say open and a site's name first.",
     "No hay ninguna pestaña en la que trabajar. Di abre y el nombre de un "
     "sitio primero."},
    {"Say what can I say to hear the commands I know.",
     "Di qué puedo decir para oír las órdenes que conozco."},

    // --- What the activation guard asks and says (activation_policy.cc).
    // What "yes" does: a window and an app are "la", a file "lo".
    {"The page tried to open a new window.",
     "La página intentó abrir una ventana nueva."},
    {"The page tried to open a new window to {0}.",
     "La página intentó abrir una ventana nueva de {0}."},
    {"open it", "abrirla"},
    {"The page tried to download a file.",
     "La página intentó descargar un archivo."},
    {"The page tried to download a file from {0}.",
     "La página intentó descargar un archivo de {0}."},
    {"download it", "descargarlo"},
    {"The page tried to open another app.",
     "La página intentó abrir otra aplicación."},
    {"The page tried to open another app with a {0} link.",
     "La página intentó abrir otra aplicación con un enlace {0}."},
    {"The page asked for something.", "La página pidió algo."},
    {"The page asked for a permission. I can't take an answer to that by "
     "voice right now, so I turned it down.",
     "La página pidió un permiso. Ahora no puedo aceptar una respuesta a eso "
     "por voz, así que lo he rechazado."},
    {"The page asked for {0} permission. I can't take an answer to that by "
     "voice right now, so I turned it down.",
     "La página pidió el permiso de {0}. Ahora no puedo aceptar una "
     "respuesta a eso por voz, así que lo he rechazado."},
    {"The page tried to go full screen. I stopped it. Say full screen if you "
     "want it.",
     "La página intentó ponerse en pantalla completa. Lo he impedido. Di "
     "pantalla completa si la quieres."},
    {"The page tried to change your clipboard. I stopped it.",
     "La página intentó cambiar tu portapapeles. Lo he impedido."},
    {"The page tried to open its video in a window over your other apps. I "
     "stopped it.",
     "La página intentó abrir su vídeo en una ventana sobre tus otras "
     "aplicaciones. Lo he impedido."},
    {"The page tried to open a new window. I stopped it.",
     "La página intentó abrir una ventana nueva. Lo he impedido."},
    {"The page tried to download a file. I stopped it.",
     "La página intentó descargar un archivo. Lo he impedido."},
    {"The page tried to open another app. I stopped it.",
     "La página intentó abrir otra aplicación. Lo he impedido."},

    // --- Questions about the page (page_questions.cc,
    // voice_page_questions.cc) -----------------------------------------------
    {"I couldn't get an answer from the page.",
     "No he podido obtener una respuesta de la página."},
    {"The page doesn't say.", "La página no lo dice."},
    {"The page doesn't seem to say anything about {0}.",
     "La página no parece decir nada sobre {0}."},
    {"The page's text tried to make me say a voice command, so I left its "
     "answer out.",
     "El texto de la página intentó hacerme decir una orden de voz, así que "
     "he omitido su respuesta."},
    {"The page says: {0}", "La página dice: {0}"},
    {"Let me look at the page.", "Voy a mirar la página."},
    {"Let me look further.", "Voy a mirar más."},
    {"Still working on it.", "Sigo con ello."},
    {"The on-device AI isn't set up. You can download a model in settings.",
     "La IA del dispositivo no está configurada. Puedes descargar un modelo "
     "en la configuración."},
    {"The on-device AI can't run on this computer, so I can't answer "
     "questions about pages.",
     "La IA del dispositivo no puede funcionar en este ordenador, así que no "
     "puedo responder preguntas sobre páginas."},
    {"I couldn't start the on-device AI. Try again in a moment, or choose "
     "another model in settings.",
     "No he podido iniciar la IA del dispositivo. Inténtalo de nuevo en un "
     "momento, o elige otro modelo en la configuración."},
    {"The on-device AI didn't answer in time, so I stopped. It may be busy, "
     "or the page too long for this computer.",
     "La IA del dispositivo no respondió a tiempo, así que he parado. Puede "
     "que esté ocupada, o que la página sea demasiado larga para este "
     "ordenador."},
    {"The on-device AI couldn't answer that. Try asking another way.",
     "La IA del dispositivo no ha podido responder a eso. Prueba a "
     "preguntarlo de otra forma."},
    {"There's no text on this page I can answer from.",
     "No hay texto en esta página con el que pueda responder."},
    {"Ask me something about this page first, then say tell me more.",
     "Pregúntame algo sobre esta página primero y luego di cuéntame más."},
    {"That's all of the page.", "Eso es toda la página."},
    {"That's all the page says about that.",
     "Eso es todo lo que dice la página sobre eso."},
    {"The page changed, so I stopped.",
     "La página ha cambiado, así que he parado."},

    // --- Reading aloud (voice_reader.cc, page_reading.cc) -------------------
    {"There's no text to read on this page.",
     "No hay texto que leer en esta página."},
    {"There's nothing to go on with here. Say read the page.",
     "Aquí no hay nada con lo que seguir. Di lee la página."},
    {"That was the end. Say read the page to hear it again.",
     "Eso era el final. Di lee la página para oírla otra vez."},
    {"Nothing is selected. Select some text, or say read the page.",
     "No hay nada seleccionado. Selecciona un texto, o di lee la página."},
    {"That's the end of the section.", "Ese es el final de la sección."},
    {"That's the end of the page.", "Ese es el final de la página."},
    {"This page has no paragraphs.", "Esta página no tiene párrafos."},
    {"That's the last heading.", "Ese es el último título."},
    {"That's the last paragraph.", "Ese es el último párrafo."},
    {"That's the first heading.", "Ese es el primer título."},
    {"That's the first paragraph.", "Ese es el primer párrafo."},
    {"This page has no sections.", "Esta página no tiene secciones."},
    {"There's no section called {0}. The sections are: {1}. Say read the "
     "section on, then its name.",
     "No hay ninguna sección llamada {0}. Las secciones son: {1}. Di lee la "
     "sección sobre y luego su nombre."},
    {"More than one section fits: {0}. Say read the section on, then its "
     "name.",
     "Hay más de una sección que encaja: {0}. Di lee la sección sobre y "
     "luego su nombre."},
    {"Reading: {0}", "Leyendo: {0}"},
    {"Done reading", "Lectura terminada"},
    {"Heading level {0}: {1}.", "Título de nivel {0}: {1}."},
    {"untitled", "sin título"},
};

}  // namespace

std::vector<SpokenAnswer> MakeSpanishSpokenAnswers() {
  return std::vector<SpokenAnswer>(std::begin(kAnswers), std::end(kAnswers));
}

}  // namespace voice
}  // namespace molt_ai
